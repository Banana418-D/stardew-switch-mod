/*
 * P0-BUILD S0 —— 游戏进程只读内存地图与预算探测
 *
 * 目的：回答一个问题 —— 已经能跑的 Mono，能不能在星露谷真实游戏进程里拿到落脚点？
 * 本文件严格只读：不初始化 Mono、不申请大块内存、不改分配器、不写任何内存页。
 *
 * 副作用只有三个：smInitialize / fsInitialize / fsOpenSdCardFileSystem（拿一个
 * 只读用的服务句柄）、在 SD 上建一个日志文件、以及一次 svcQueryMemory 全图扫描。
 * 不写任何内存页、不改任何页权限、不建线程、不创建 JIT 对象、不碰任何分配器。
 *
 * ---------------------------------------------------------------------------
 * 注入方式（不改 sdv_native 的任何现有文件）
 * ---------------------------------------------------------------------------
 * 把本文件放到：sdv_native/source/lib/p0_s0_probe.c
 *
 * 依据 misc/mk/common.mk：
 *     MODULES := $(shell find $(ROOT_SOURCE) -mindepth 1 -maxdepth 1 -type d)
 *     SOURCES := $(foreach module,$(MODULES),$(shell find $(module) -type d))
 *     CFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/x.c)))
 * source/lib 本身就在 SOURCES 里，所以这个 .c 会被自动通配到手，不需要改 Makefile。
 *
 * 触发点：source/lib/init/init.cpp 的 exl_module_init() 里有一句 __init_array()，
 * 它遍历 .init_array 段。__attribute__((constructor)) 正是把函数指针放进这个段，
 * 所以本探针在 exl_init()（内存布局/patcher/版本都就绪）之后自动执行，
 * 不需要在 init.cpp 里加任何调用。
 *
 * 移除：删掉这一个文件，重新 make 即恢复原样。git status 只会看到它一个 untracked。
 *
 * ---------------------------------------------------------------------------
 * 输出：sdmc:/p0-s0.log（SD 根不可写时退到 /switch/p0-s0.log）
 * ---------------------------------------------------------------------------
 * 只走裸 `fs` 服务，**绝不用 fsdevMountSdmc + fopen**。这不是偏好，是硬约束：
 *
 *   fsdevMountSdmc -> fs_dev.o -> envIsNso -> env.o -> __nx_exit -> switch_crt0.o
 *   switch_crt0.o 在 -nostartfiles 的模块里会报
 *   "R_AARCH64_PREL32 against __eh_frame_hdr_start ... recompile with -fPIC"
 *
 * 这条链在本文件的第一版上真实触发过 —— 整个 sdv_native 链接当场失败。
 * sdv_native 自己的 source/program/sdlog.cpp 早就踩过同一个坑，注释里写着
 * 同样的结论，它用的正是下面这套裸 fs 调用。
 *
 * 写入策略：自己攒 4 KB 缓冲再落盘。fsFileWrite 单次开销在毫秒级，
 * 几千段地图如果每行一次 syscall，会把游戏启动拖到几十秒。
 */

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

/* 链接脚本提供的段边界（misc/link.ld）。注意 __start__ = 0x0，
 * 这些符号是**模块内偏移**，不是运行时 VA。 */
extern char __text_start__[], __text_end__[];
extern char __bss_start__[], __bss_end__[];
extern char __end__[];

#define S0_PATH_RAW  "/p0-s0.log"          /* SD 根 */
#define S0_PATH_ALT  "/switch/p0-s0.log"   /* 根不可写时的退路 */

/* 段数上限：ASLR 区间动辄几十 GB，正常几百到几千段。
 * 超过就截断并如实标注 —— 宁可少写，不能把启动卡死。 */
#define S0_MAX_SEGS  4096

/* 候选块判据（PLAN v2 §3） */
#define ALIGN_BIG    (64ull * 1024 * 1024)   /* 1 GiB 块的起始对齐要求 */
#define SIZE_1G      (1024ull * 1024 * 1024)
#define SIZE_512M    (512ull * 1024 * 1024)
#define SIZE_256M    (256ull * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* 日志通道                                                           */
/* ------------------------------------------------------------------ */

#define LOG_BUF 4096

static FsFileSystem g_fs;
static FsFile       g_file;
static s64          g_off;
static bool         g_open;
static const char  *g_path;
static char         g_buf[LOG_BUF];
static u32          g_buf_len;

/* 把缓冲里的内容真正写下去。失败就置 g_open=false，后续全部静默 ——
 * 探针绝不能因为写不进日志而影响游戏。 */
static void log_flush(void)
{
    u32 done = 0;

    if (!g_open || g_buf_len == 0) {
        return;
    }
    while (done < g_buf_len) {
        if (R_FAILED(fsFileWrite(&g_file, g_off + (s64)done,
                                 g_buf + done, g_buf_len - done,
                                 FsWriteOption_Flush))) {
            g_open    = false;
            g_buf_len = 0;
            return;
        }
        done += g_buf_len - done;
    }
    g_off += (s64)g_buf_len;
    g_buf_len = 0;
}

static void s0_log(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    int n;

    if (!g_open) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(line) - 2) {
        n = (int)sizeof(line) - 2;
    }
    line[n++] = '\n';

    /* 单行超长时先落盘，保证不跨缓冲区撕裂 */
    if (g_buf_len + (u32)n > LOG_BUF) {
        log_flush();
        if (!g_open) {
            return;
        }
    }
    memcpy(g_buf + g_buf_len, line, (size_t)n);
    g_buf_len += (u32)n;
}

/* 试一个目录：建文件 + 打开 + 截断，全过才算可用 */
static bool log_try_dir(const char *path)
{
    FsFile f;

    fsFsCreateFile(&g_fs, path, 0, 0);   /* 已存在会失败，忽略 */
    if (R_FAILED(fsFsOpenFile(&g_fs, path, FsOpenMode_Write, &f))) {
        return false;
    }
    if (R_FAILED(fsFileSetSize(&f, 0))) {
        fsFileClose(&f);
        return false;
    }
    g_file    = f;
    g_off     = 0;
    g_buf_len = 0;
    g_path    = path;
    g_open    = true;
    return true;
}

static void log_open(void)
{
    /* libnx 的 fsInitialize 内部会调 smInitialize；两者都是引用计数，
     * 与 sdv_native 自己的 sdlog 并存不会互相破坏。 */
    smInitialize();
    if (R_FAILED(fsInitialize())) {
        return;
    }
    if (R_FAILED(fsOpenSdCardFileSystem(&g_fs))) {
        return;
    }

    /* 有些 Atmosphere 配置对 app 进程隐藏 SD 根，但 /switch 可达 */
    if (!log_try_dir(S0_PATH_RAW)) {
        if (!log_try_dir(S0_PATH_ALT)) {
            fsFsClose(&g_fs);
        }
    }
}

static void log_close(void)
{
    if (g_open) {
        log_flush();
        fsFileClose(&g_file);
        g_open = false;
    }
    if (g_fs.s.session != 0) {
        fsFsClose(&g_fs);
        g_fs.s.session = 0;
    }
}

/* ------------------------------------------------------------------ */
/* svcGetInfo 包装                                                    */
/* ------------------------------------------------------------------ */

static void info1(const char *name, u32 id)
{
    u64 v   = 0;
    Result rc = svcGetInfo(&v, id, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(rc)) {
        s0_log("info %-26s = 0x%llX  (%llu MiB)",
             name, (unsigned long long)v, (unsigned long long)(v >> 20));
    } else {
        /* 失败也必须记 —— "这个平台没这个字段"本身是结论 */
        s0_log("info %-26s FAIL rc=%08X", name, (unsigned)rc);
    }
}

/* ------------------------------------------------------------------ */
/* 空闲块统计                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    u64 start;
    u64 len;
} Block;

static Block g_big3[3];        /* 最大的 3 个连续空闲块 */
static u64   g_free_total;     /* 未映射总量 */
static u64   g_free_segs;
static u64   g_mapped_segs;
static u64   g_cand_1g;
static u64   g_cand_512m;
static u64   g_cand_256m;

static void block_account(u64 start, u64 len)
{
    int i, worst = -1;
    u64 worst_len = ~0ull;

    if (len == 0) {
        return;
    }
    g_free_total += len;
    g_free_segs++;

    if (len >= SIZE_1G && (start & (ALIGN_BIG - 1)) == 0) {
        g_cand_1g++;
    }
    if (len >= SIZE_512M) {
        g_cand_512m++;
    }
    if (len >= SIZE_256M) {
        g_cand_256m++;
    }

    for (i = 0; i < 3; i++) {
        if (g_big3[i].len < worst_len) {
            worst_len = g_big3[i].len;
            worst     = i;
        }
    }
    if (worst >= 0 && len > g_big3[worst].len) {
        g_big3[worst].start = start;
        g_big3[worst].len   = len;
    }
}

/* ------------------------------------------------------------------ */
/* 主扫描                                                             */
/* ------------------------------------------------------------------ */

static void scan_map(void)
{
    u64 base = 0, size = 0;
    Result rc;
    u64 addr;
    u64 end;
    u64 segs = 0;
    int truncated = 0;
    u64 run_start = 0, run_len = 0;

    rc = svcGetInfo(&base, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc)) {
        s0_log("map: AslrRegionAddress FAIL rc=%08X -- 不猜范围，扫描中止", (unsigned)rc);
        return;
    }
    rc = svcGetInfo(&size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc)) {
        s0_log("map: AslrRegionSize FAIL rc=%08X -- 扫描中止", (unsigned)rc);
        return;
    }

    addr = base;
    end  = base + size;
    s0_log("map: region [0x%llX, 0x%llX) size=%llu MiB",
         (unsigned long long)base, (unsigned long long)end,
         (unsigned long long)(size >> 20));
    s0_log("map: columns = addr size type perm attr");
    s0_log("--- segments ---");

    while (addr < end) {
        MemoryInfo mi;
        u32 pi = 0;
        u64 next;

        if (segs >= S0_MAX_SEGS) {
            truncated = 1;
            break;
        }

        memset(&mi, 0, sizeof(mi));
        rc = svcQueryMemory(&mi, &pi, addr);
        if (R_FAILED(rc) || mi.size == 0) {
            s0_log("map: query FAIL at 0x%llX rc=%08X size=%llu -- 步进 4K 继续",
                 (unsigned long long)addr, (unsigned)rc,
                 (unsigned long long)mi.size);
            /* 保证前进，绝不死循环 */
            addr += 0x1000;
            continue;
        }

        /* 未映射 = type 0 且 perm 0。已映射但 perm=0 的（如保留区）要分开看。 */
        if (mi.type == MemType_Unmapped && mi.perm == 0) {
            if (run_len == 0) {
                run_start = mi.addr;
            }
            run_len += mi.size;
            s0_log("0x%016llX %10lluK FREE",
                 (unsigned long long)mi.addr,
                 (unsigned long long)(mi.size >> 10));
        } else {
            if (run_len != 0) {
                block_account(run_start, run_len);
                run_len = 0;
            }
            g_mapped_segs++;
            s0_log("0x%016llX %10lluK type=%02X perm=%X attr=%X refs=%u/%u",
                 (unsigned long long)mi.addr,
                 (unsigned long long)(mi.size >> 10),
                 (unsigned)mi.type, (unsigned)mi.perm, (unsigned)mi.attr,
                 (unsigned)mi.ipc_refcount, (unsigned)mi.device_refcount);
        }

        segs++;

        /* 关键：mi.addr 可能小于 addr（addr 落在段中间），必须取两者较大值推进 */
        next = mi.addr + mi.size;
        if (next <= addr) {
            next = addr + 0x1000;
        }
        addr = next;
    }

    if (run_len != 0) {
        block_account(run_start, run_len);
    }

    s0_log("--- summary ---");
    s0_log("summary: segments=%llu%s", (unsigned long long)segs,
         truncated ? " (TRUNCATED at cap, region not fully scanned)" : "");
    s0_log("summary: mapped_segs=%llu free_concat_segs=%llu free_total=%llu MiB",
         (unsigned long long)g_mapped_segs, (unsigned long long)g_free_segs,
         (unsigned long long)(g_free_total >> 20));
    s0_log("summary: candidates >=1GiB&64MiB-aligned = %llu", (unsigned long long)g_cand_1g);
    s0_log("summary: candidates >=512MiB = %llu", (unsigned long long)g_cand_512m);
    s0_log("summary: candidates >=256MiB = %llu", (unsigned long long)g_cand_256m);

    for (int i = 0; i < 3; i++) {
        if (g_big3[i].len != 0) {
            s0_log("summary: top%d base=0x%llX size=%llu MiB align64M=%s",
                 i + 1,
                 (unsigned long long)g_big3[i].start,
                 (unsigned long long)(g_big3[i].len >> 20),
                 (g_big3[i].start & (ALIGN_BIG - 1)) == 0 ? "yes" : "no");
        }
    }
}

/* ------------------------------------------------------------------ */
/* 模块自身与 jit 能力                                                */
/* ------------------------------------------------------------------ */

static void probe_self(void)
{
    /* linker 符号是模块内偏移（__start__ = 0x0），不是 VA。
     * 顺手查一下本函数所在的段，给出"模块被 rtld 放在哪"。 */
    u64 fn = (u64)(uintptr_t)&probe_self;
    MemoryInfo mi;
    u32 pi = 0;

    s0_log("--- self ---");
    s0_log("self: text  off=[0x%llX,0x%llX) size=%lluK",
         (unsigned long long)(uintptr_t)__text_start__,
         (unsigned long long)(uintptr_t)__text_end__,
         (unsigned long long)(((uintptr_t)__text_end__ - (uintptr_t)__text_start__) >> 10));
    s0_log("self: bss   off=[0x%llX,0x%llX) size=%lluK",
         (unsigned long long)(uintptr_t)__bss_start__,
         (unsigned long long)(uintptr_t)__bss_end__,
         (unsigned long long)(((uintptr_t)__bss_end__ - (uintptr_t)__bss_start__) >> 10));
    s0_log("self: end   off=0x%llX", (unsigned long long)(uintptr_t)__end__);
    s0_log("self: probe_self VA=0x%llX", (unsigned long long)fn);

    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, fn))) {
        s0_log("self: containing seg base=0x%llX size=%lluK type=%02X perm=%X",
             (unsigned long long)mi.addr,
             (unsigned long long)(mi.size >> 10),
             (unsigned)mi.type, (unsigned)mi.perm);
        /* 模块基址 = 段基址 - 该段在模块内的偏移。text 段通常从 0 开始，
         * 所以段基址近似模块加载基址；这里只做记录，不做假设。 */
        s0_log("self: module_base_hint=0x%llX (seg base; text starts at offset 0)",
             (unsigned long long)mi.addr);
    } else {
        s0_log("self: svcQueryMemory(self) FAILED");
    }
}

/*
 * 这里原本有一个 jitCreate(16K) 探针，已删除 —— 两个理由，都要记在账上：
 *
 * 1. 裁定边界。本轮"只允许"清单里只有查询内存区域/权限、额度、ASLR、
 *    映射探针的可行性验证，没有 JIT 创建。PLAN v2 里列过它，但这一次的
 *    裁定收得更紧，按裁定执行。
 *
 * 2. 它根本链不进来。jitCreate -> jit.o -> envIsSyscallHinted /
 *    envGetOwnProcessHandle -> env.o -> __nx_exit -> switch_crt0.o，
 *    而 switch_crt0.o 在 -nostartfiles 的模块里必报
 *    "R_AARCH64_PREL32 against __eh_frame_hdr_start ... recompile with -fPIC"。
 *    这是构建过程中真实撞到的墙，不是推测。
 *
 * 更要紧的是第 2 条背后的语义：env.o 的 g_processHandle 由 crt0 填充，
 * subsdk8 模块里从来没有 crt0，所以即使把符号桩上，envGetOwnProcessHandle()
 * 也只会返回 0，jitCreate 拿 0 当 own process handle 去 svcMapProcessCodeMemory，
 * 必然失败。**jitCreate 这条路在 subsdk8 形态下大概率不可用。**
 *
 * 这条结论直接冲击 S1：mono 的 mono-codeman-libnx.c 全靠 jitCreate 拿 RW/RX
 * 视图。S1 之前必须先回答"可执行内存从哪来"。sdv_native 自己是 inline hook，
 * 它一定已经有答案 —— 那是 S1 的第一个调研点，不是本文件的事。
 */

/* ------------------------------------------------------------------ */
/* 入口                                                               */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void p0_s0_probe(void)
{
    log_open();

    if (!g_open) {
        /* 没有任何落盘通道。此时唯一能做的事是不干扰游戏。
         * 下次进来若看到 p0-s0.log 不存在，就说明两个后端都没通。 */
        return;
    }

    s0_log("=== P0 S0 read-only memory probe ===");
    s0_log("path=%s", g_path != NULL ? g_path : "(none)");
    s0_log("build=%s %s", __DATE__, __TIME__);

    s0_log("--- svcGetInfo ---");
    info1("AliasRegionAddress",        InfoType_AliasRegionAddress);
    info1("AliasRegionSize",           InfoType_AliasRegionSize);
    info1("HeapRegionAddress",         InfoType_HeapRegionAddress);
    info1("HeapRegionSize",            InfoType_HeapRegionSize);
    info1("TotalMemorySize",           InfoType_TotalMemorySize);
    info1("UsedMemorySize",            InfoType_UsedMemorySize);
    info1("AslrRegionAddress",         InfoType_AslrRegionAddress);
    info1("AslrRegionSize",            InfoType_AslrRegionSize);
    info1("StackRegionAddress",        InfoType_StackRegionAddress);
    info1("StackRegionSize",           InfoType_StackRegionSize);
    info1("SystemResourceSizeTotal",   InfoType_SystemResourceSizeTotal);
    info1("SystemResourceSizeUsed",    InfoType_SystemResourceSizeUsed);
    info1("TotalNonSystemMemorySize",  InfoType_TotalNonSystemMemorySize);
    info1("UsedNonSystemMemorySize",   InfoType_UsedNonSystemMemorySize);
    info1("AddressSpaceSize",          InfoType_AddressSpaceSize);

    probe_self();
    scan_map();

    s0_log("=== S0 done ===");
    log_close();
}
