#include "program/sdlog.hpp"

/* Deliberately includes only libnx, not the exlaunch headers: both define
 * u8/u32/u64 and they cannot be mixed in one translation unit.
 *
 * Uses the raw `fs` service API rather than fsdevMountSdmc()/fopen().  The
 * newlib route pulls libnx's switch_crt0.o, which cannot be linked into a
 * `-nostartfiles` exlaunch module (dangerous relocation against
 * __eh_frame_hdr_start etc.). */
#include <switch.h>

#include <cstdarg>
#include <cstdio>

namespace sdlog {
namespace {

FsFileSystem g_fs{};
FsFile g_file{};
bool g_open = false;
/* A filesystem is mounted and a directory inside it has been proven writable.
 * Deliberately separate from g_open: the mount can succeed while every file
 * operation still fails, and that distinction is the whole point of the probe
 * below. */
bool g_fs_ready = false;
/* 0 = nothing, 1 = SD root, 2 = SD /switch, 3 = save data. */
int g_chan = 0;
/* Directory prefix inside the mounted filesystem ("" or "/sdv_native").  v565
 * moved every file this project leaves on the card into one folder: the SD root
 * was accumulating sdv_map.log, sdv_log.txt, sdv_map.txt, sdv_items.cfg and a
 * dozen sdv_0N_* markers right next to the game's own files, and "copy the
 * whole folder" is one gesture instead of picking files out of a list.
 * Falls back to "" (root) when the folder cannot be created, because a missing
 * log is worse than a messy root. */
const char* g_dir = "";
const char kFolder[] = "/sdv_native";
/* v570: logs and settings are separated.  Everything that is written to help a
 * report (the log, the boot markers, the hung-step markers) goes into
 * /sdv_native/logs, so "clear the logs" is deleting one folder; the settings
 * file stays one level up, in /sdv_native, and survives that. */
const char kLogDir[] = "/sdv_native/logs";
char g_pathbuf[96] = {0};
/* Result codes of each step, so a failed mount can be reported on screen when
 * there is no log file to read.  0 = that step succeeded.
 *   [0] fsInitialize        [1] fsOpenSdCardFileSystem
 *   [2] last fsFsCreateFile [3] smInitialize
 *   [4] fsOpen_SaveData     */
unsigned g_diag[5] = {0, 0, 0, 0, 0};

/* SDLOG-2: results of the two calls that actually decide whether the write
 * channel exists, plus a phase cursor.  0xFFFFFFFF = never reached.
 *
 * SDLOG-1 left 0-byte probe files in both the SD root and /switch: the create
 * succeeded and whatever came after it did not.  Those two codes say which. */
unsigned g_of_rc = 0xFFFFFFFFu;
unsigned g_wr_rc = 0xFFFFFFFFu;
volatile unsigned g_phase = 0;
/* The tag was written and the file closed; Open() counts as done even though
 * no handle is held.  Kept separate from g_open so Line() stays a no-op while
 * the "wrote once" state is still readable on screen. */
bool g_wrote = false;
/* v647: the fs channel is opened at most ONCE per process.  v646 reopened it
 * after every long menu / trip to the title (the log shows five separate
 * session headers and five different file sizes in one play session), which
 * means smInitialize/fsInitialize/fsOpenSdCardFileSystem and their Exit
 * counterparts ran five times.  A console flash-exit with no erpt report
 * followed.  Repeatedly cycling service sessions is the only new behaviour
 * v646 added, so remove it: one open, at most one close, then stay in the
 * zero-fs-session state that v645 proved safe for good. */
bool g_dead = false;
/* This session created the log file rather than finding one.  It matters at
 * step 7: create(length) only *allocates* that many bytes, it does not zero
 * them, so getSize on a file we just made reports the requested length and the
 * bytes behind it are whatever the cluster held before.  SDLOG-7 trusted that
 * number and appended the header after seven bytes of garbage. */
bool g_fresh = false;

/* ---------------------------------------------------------------------------
 * Session quota  (v529)
 *
 * smInitialize() is svcConnectToNamedPort("sm:").  On the kernel side
 * KClientPort::CreateSession *first* reserves one SessionCountMax slot from
 * the calling process's own KResourceLimit and only then allocates the
 * KSession.  An application's limit for that counter is 1, and the game's own
 * nn::sf has already spent it, so the reserve fails and the SVC returns
 * 0x10801 -- which is LimitReached, not OutOfResource (0xCE01 is the object
 * pool, and it has never appeared).  Every libnx route into fs, save data or
 * an SD card funnels through that same one slot, which is why all of them
 * failed identically.
 *
 * The counter is writable from inside the process.  KResourceLimit::
 * SetLimitValue only rejects "current_used > new_limit", so raising is legal,
 * and the deployed main.npdm grants svcSetResourceLimitLimitValue (0x7E).
 * Raising it costs nothing -- it is a counter, not memory -- and it leaves the
 * game headroom rather than taking its last slot.
 *
 * Two traps in this SVC pair:
 *   - svcGetInfo(InfoType_ResourceLimit) requires handle == 0 (the kernel
 *     compares against svc::InvalidHandle, which is literal 0).  Passing
 *     CUR_PROCESS_HANDLE (0xFFFF8001) fails with 0xE401.
 *   - The kernel adds a brand-new handle on every call, so the caller owns it
 *     and must close it.  Leaking one per attempt would eat the handle table.
 * ------------------------------------------------------------------------ */
unsigned g_diag_rl[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
bool g_quota_done = false;
bool g_sm_ok = false;

void RaiseSessionQuota() {
    if (g_quota_done) {
        return;
    }
    g_quota_done = true;

    u64 h = 0;
    const unsigned irc = svcGetInfo(&h, InfoType_ResourceLimit, 0, 0);
    g_diag_rl[0] = irc;
    if (R_FAILED(irc) || h == 0 || h == 0xFFFFFFFFull) {
        return;
    }
    const Handle rl = static_cast<Handle>(h);

    s64 cur = 0;
    s64 lim = 0;
    if (R_SUCCEEDED(
            svcGetResourceLimitCurrentValue(&cur, rl, LimitableResource_Sessions))) {
        g_diag_rl[1] = static_cast<unsigned>(cur);
    }
    if (R_SUCCEEDED(
            svcGetResourceLimitLimitValue(&lim, rl, LimitableResource_Sessions))) {
        g_diag_rl[2] = static_cast<unsigned>(lim);
    }

    /* Raise with room to spare.  SDLOG-1 sized this at "used + 4" and that is
     * exactly the number of sessions the write path needs by itself:
     *   sm: (1) + fsp-srv (1) + IFileSystem for the SD card (1)
     *   + IFile for every opened file (1 each)
     * The game opens sessions concurrently, so a limit that is merely "just
     * enough" can lose the race -- and the symptom of that is precisely what
     * was observed: fsFsCreateFile (no new session, reuses IFileSystem)
     * succeeds while fsFsOpenFile (new IFile session) fails, leaving a 0-byte
     * file behind.  The counter is a cap, not an allocation, so slack is free.
     * Note the check is "limit < used + 8", not "used >= limit": before the
     * game's own nn::sf connects, used is still 0 and limit is 1. */
    if (g_diag_rl[1] != 0xFFFFFFFFu && g_diag_rl[2] != 0xFFFFFFFFu &&
        lim < cur + 8) {
        u64 want = static_cast<u64>(cur) + 16u;
        if (want < 16u) {
            want = 16u;
        }
        g_diag_rl[3] = svcSetResourceLimitLimitValue(
            rl, LimitableResource_Sessions, want);
    }

    /* Hand the borrowed handle back: it was created for us by svcGetInfo. */
    svcCloseHandle(rl);
}

int AppendStr(char* out, int cap, int n, const char* s) {
    if (s != nullptr) {
        for (const char* p = s; *p != '\0' && n < cap - 1; p++) {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return n;
}

/* `dir` may be "" (filesystem root) or "/switch". */
bool JoinPath(const char* dir, const char* name, char* out, int cap) {
    if (out == nullptr || name == nullptr || cap < 8) {
        return false;
    }
    out[0] = '\0';
    int n = AppendStr(out, cap, 0, (dir != nullptr) ? dir : "");
    n = AppendStr(out, cap, n, "/");
    n = AppendStr(out, cap, n, name);
    return n > 1;
}

/* `out` receives g_dir + "/" + name. */
bool MakePath(char* out, int cap, const char* name) {
    if (!g_fs_ready) {
        return false;
    }
    return JoinPath(g_dir, name, out, cap);
}

/* Replaces a small file.  Uses the same recipe the log needed on hardware:
 * create at the final length, extend explicitly, write with the flush option.
 * The old create(0)+write(0) version is what silently produced 0-byte files,
 * and this same helper backs sdv_map.cfg / sdv_keep.cfg -- a setting that
 * never persists looks exactly like a setting the game overwrote. */
bool WriteSmall(const char* path, const char* data, u64 len) {
    fsFsDeleteFile(&g_fs, path);
    const unsigned cr =
        fsFsCreateFile(&g_fs, path, static_cast<s64>(len), 0);
    g_diag[2] = cr;
    if (R_FAILED(cr)) {
        return false;
    }
    FsFile f{};
    if (R_FAILED(fsFsOpenFile(&g_fs, path, FsOpenMode_Write, &f))) {
        return false;
    }
    fsFileSetSize(&f, static_cast<s64>(len));
    const unsigned wr =
        fsFileWrite(&f, 0, data, len, FsWriteOption_Flush);
    fsFileFlush(&f);
    fsFileClose(&f);
    return R_SUCCEEDED(wr);
}

/* Returns true only when `len` bytes came back and the first one matches. */
bool ReadSmall(const char* path, char* out, u64 len) {
    FsFile f{};
    if (R_FAILED(fsFsOpenFile(&g_fs, path, FsOpenMode_Read, &f))) {
        return false;
    }
    char tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    u64 got = 0;
    const unsigned rr = fsFileRead(&f, 0, tmp, (len < 8u) ? len : 8u, 0, &got);
    fsFileClose(&f);
    if (R_FAILED(rr) || got < len) {
        return false;
    }
    for (u64 i = 0; i < len; i++) {
        out[i] = tmp[i];
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Stepped open  (SDLOG-4)  --  the SD card is the only channel
 *
 * Every step runs on the game's own thread, one step per frame.
 *
 * SDLOG-2 put all seven calls on a bare svcCreateThread thread so that a
 * blocking call could not freeze the picture.  It crashed the title instead:
 * every libnx IPC builds its request in the buffer armGetTls() returns, i.e.
 * TPIDRRO_EL0, which is per-thread and set by the kernel -- a raw thread
 * cannot be given one, so it wrote into the game's cmif buffer.  No second
 * thread.  IPC stays on the thread that owns the process's TLS.
 *
 * Two hardware facts shaped this version:
 *   1. The on-screen readout has never drawn anything.  Its block is gated on
 *      `kMapStage == 14` while kMapStage is 327, so the compiler folds it away
 *      -- main.o references no sdlog::Diag* symbol at all.  Nothing that only
 *      exists on screen can be trusted; the SD card is the channel.
 *   2. fsFsCreateFile demonstrably works: SDLOG-1's probe file, SDLOG-2's and
 *      SDLOG-3's sdv_map.log all appeared.  The bytes never arrive -- SDLOG-3
 *      played a full session and left the file at 0 bytes -- so create is the
 *      one operation a diagnostic can be built on.
 *
 * Therefore every step drops a marker file whose NAME carries the result:
 *
 *   sdv_01_q_<info>_<cur>_<lim>_<set>   session-quota probe (4 codes)
 *   sdv_02_sm_<rc>                      smInitialize
 *   sdv_03_fs_<rc>                      fsInitialize
 *   sdv_04_sd_<rc>                      fsOpenSdCardFileSystem
 *   sdv_04b_dir_<rc>                    create the /sdv_native folder (v565)
 *   sdv_05_cr_<rc>                      create the log (0x402 = it already exists, fine)
 *   sdv_06_op_<rc> / sdv_06b_ap_<rc>    open Write / open Write|Append
 *   sdv_07_sz_<size>                    size the session appends at
 *   sdv_07_rc_<create>_<open>           recycle path, only when that size hit 2 MB
 *   sdv_07_ss_<rc>                      fsFileSetSize
 *   sdv_08_wr_<rc>                      fsFileWrite with FsWriteOption_Flush
 *   sdv_09_fl_<rc>                      fsFileFlush
 *   sdv_10_ok                           written and flushed
 *   sdv_p5..sdv_p9                      dropped *before* the step runs
 *
 * Reading a directory does not use the IFile path, so the markers survive a
 * failure of open/write/flush, and the last `sdv_pN` names the call that hung.
 * Steps 1-3 run before any filesystem exists; their codes are buffered and
 * emitted in the 01..04 batch as soon as step 4 succeeds.
 *
 * g_phase still tracks the step for the record (1 quota .. 9 flush, 0xA done,
 * 0xE? + step on failure), but nothing reads it on screen any more.
 * ------------------------------------------------------------------------ */
unsigned g_step = 0;
bool g_failed = false;

/* SDLOG-4 proved the recipe on hardware: the file must already be long enough
 * before the bytes go in.  Creating it at length 0 and writing at offset 0 is
 * what left two builds' worth of 0-byte logs behind; create-at-length plus
 * fsFileSetSize plus a flush-on-write option wrote "SDLOG4\n" on the first
 * try, with every step returning 0 and cur=1/lim=17 on the session counter
 * (so it was never a quota problem).  Every append below repeats exactly that
 * sequence: extend to the end offset, then write at it. */
const char kHdr[] = "\n-- SDLOG11 --\n";
constexpr s64 kHdrLen = static_cast<s64>(sizeof(kHdr) - 1);

/* The log accumulates across launches instead of being rotated.
 *
 * SDLOG-6 tried to rename the old log to sdv_map.prev.log before creating a new
 * one, and hardware answered fsFsRenameFile with 0xD401 -- that is not an fs
 * error at all: module 1 (kernel), description 106, svc::ResultInvalidCurrent-
 * Memory, "passed buffer is not usable".  Whatever the fsp path for rename
 * wants, this process cannot give it, so rename is off the table.
 *
 * Appending is better anyway: the interesting part of a crash log is the tail,
 * and a tail only survives if the next launch writes after it rather than over
 * it.  Each session appends its own header line, so the newest session always
 * starts at the last "SDLOG7" in the file.  The only recycling left is a size
 * check at open: past 2 MB the file is deleted and started over, and delete is
 * the one fs call hardware has never refused. */
const char kLogName[] = "sdv_map.log";

/* Appends are not free -- open, get-size, set-size, write, flush, close is six
 * IPCs -- and LOGF is called from paths that can run more than once a frame.
 * A token bucket keeps a runaway path from turning into a stutter, and the
 * caps stop a long session from filling the card.  svcGetSystemTick is already
 * used by exlaunch's RNG, so it is a granted call in this process. */
constexpr int kTokensBurst = 12;
constexpr u64 kTickHz = 19200ull;                 /* 19.2 MHz */
constexpr u64 kRefillTicks = kTickHz * 2000ull;   /* bucket refills every 2 s */
/* SDLOG-5 shipped 400 lines / 128 KB and the first real run hit the cap at
 * exactly 400 lines -- all of them the automation heartbeat, which fires twice
 * per eight frames.  The budget was spent in about a minute and every later
 * line, including anything worth reading, was dropped in silence.  So: a cap
 * that no realistic session reaches, a heartbeat that is charged as a
 * low-priority line (it may never take the last half of the bucket), and a
 * marker file the moment the cap is hit, because "the log just stops" is
 * indistinguishable from "the game stopped logging". */
constexpr unsigned kCapLines = 400000;
constexpr s64 kCapBytes = 16 * 1024 * 1024;
u64 g_lastTick = 0;
int g_tokens = kTokensBurst;
unsigned g_drops = 0;
unsigned g_lines = 0;
bool g_cap_marked = false;
s64 g_logBytes = 0;
/* One marker per session is enough to describe a broken append; a marker per
 * failed line would flood the card with exactly the noise being debugged. */
bool g_wr_fail_marked = false;

/* v613: the write gate.  False means "no world loaded, keep the card quiet".
 *
 * Hardware froze on the character-creation screen: music still playing, the
 * picture stopped, and the log ends on an ordinary automation line.  That
 * screen hands the display to the system software keyboard, i.e. an applet
 * switch, and this log spends six fs IPCs per line.  An IPC that never comes
 * back freezes the main thread while the audio thread keeps going -- exactly
 * the observed shape.  Nothing worth logging happens before a location
 * exists, so the update hook closes the gate until then. */
bool g_gate = true;

/* v641: the held write handle used to be a function-local static inside
 * LineImpl.  SetGate(false) needs to close it, so it lives here now.
 *
 * This is the missing half of the v613 note above: SetGate() existed and its
 * comment said "the update hook closes the gate", but the function had ZERO
 * callers in main.cpp.  Every build from v613 to v640 therefore kept an open
 * FsFile and kept appending `gate:` lines during the character-creation
 * screen.  v617 installed no hooks at all, so sdlog::Step() never ran and no
 * fs session was ever opened -- which is why that one build never froze. */
FsFile s_file{};
bool   s_fileOk = false;
unsigned s_sinceFlush = 0;

/* Close the handle before the system software keyboard takes the display.
 * An open file plus an in-flight fs IPC across an applet switch is the shape
 * the v613 comment was written to prevent. */
void CloseHandle() {
    if (s_fileOk) {
        fsFileFlush(&s_file);
        fsFileClose(&s_file);
        s_fileOk = false;
        s_sinceFlush = 0;
    }
}

/* `low` marks chatter that is nice to have but must never crowd out an event.
 * A low-priority line is refused while the bucket is at or below half, so a
 * chatty heartbeat can at worst use the slack and never the reserve.  SDLOG-5
 * had no such distinction and its heartbeat consumed the entire budget. */
bool TakeLineSlot(bool low) {
    const u64 now = svcGetSystemTick();
    if (g_lastTick == 0 || now - g_lastTick >= kRefillTicks) {
        g_lastTick = now;
        g_tokens = kTokensBurst;
    }
    if (g_tokens <= 0 || (low && g_tokens <= kTokensBurst / 2)) {
        ++g_drops;
        return false;
    }
    --g_tokens;
    return true;
}

/* Best effort: a marker that cannot be created is itself information (create
 * broke too), but it must never stop the sequence. */
void Marker(const char* name) {
    if (!g_fs_ready || name == nullptr) {
        return;
    }
    char p[96];
    if (!JoinPath(g_dir, name, p, static_cast<int>(sizeof(p)))) {
        return;
    }
    fsFsDeleteFile(&g_fs, p);
    fsFsCreateFile(&g_fs, p, 1, 0);
}

void MarkerF(const char* fmt, ...) {
    char name[64];
    std::va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(name, sizeof(name), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    Marker(name);
}

/* One marker per session for a broken append: the whole point of the log is to
 * be readable, and a marker per failed line would bury the card in the very
 * failure it is reporting. */
void NoteWriteFailure(unsigned rc) {
    if (g_wr_fail_marked) {
        return;
    }
    g_wr_fail_marked = true;
    MarkerF("sdv_wr_fail_%08x", rc);
}

/* Exactly one blocking call per invocation; the caller runs this once a frame. */
void StepOnce() {
    switch (g_step) {
        case 0:
            /* Announce before the first SVC.  The frame that draws "1" is
             * drawn before the quota call runs, so even a hang inside the very
             * first call leaves a number on screen. */
            g_phase = 1;
            g_step = 1;
            return;

        case 1:
            RaiseSessionQuota();
            g_phase = 2;
            g_step = 2;
            return;

        case 2: {
            /* libnx's fsInitialize goes straight to smGetService("fsp-srv"),
             * which dispatches through g_smSrv without checking whether that
             * session was ever created.  After a failed init g_smSrv is still
             * zeroed, so calling fsInitialize anyway sends a sync request to
             * handle 0.  Bail out at the sm: failure instead. */
            const unsigned rc = smInitialize();
            g_diag[3] = rc;
            if (R_FAILED(rc)) {
                g_phase = 0xE2;
                g_failed = true;
                return;
            }
            g_sm_ok = true;
            g_phase = 3;
            g_step = 3;
            return;
        }

        case 3: {
            const unsigned rc = fsInitialize();
            g_diag[0] = rc;
            if (R_FAILED(rc)) {
                g_phase = 0xE3;
                g_failed = true;
                return;
            }
            g_phase = 4;
            g_step = 4;
            return;
        }

        case 4: {
            const unsigned sd = fsOpenSdCardFileSystem(&g_fs);
            g_diag[1] = sd;
            if (R_FAILED(sd)) {
                g_phase = 0xE4;
                g_failed = true;
                return;
            }
            /* SD root only.  /switch and the save-data channel are gone:
             * hardware showed the probe failing identically in root and
             * /switch (two 0-byte files), and the save-data fallback is what
             * froze the console on SDLOG-1 -- fsOpen_SaveData blocks forever
             * while the game already holds that save, which is true at every
             * moment after the title screen. */
            g_chan = 1;
            g_dir = "";
            g_fs_ready = true;
            /* v565/v570: logs live in /sdv_native/logs so the settings file one
             * level up is not deleted along with them.  0x402 = "already
             * exists", the normal answer from the second launch on.  Any other
             * failure leaves g_dir alone, so the files land in the root exactly
             * as they used to -- a card that cannot take a directory must not
             * also lose the log. */
            const unsigned mkParent = fsFsCreateDirectory(&g_fs, kFolder);
            const unsigned mk =
                (R_SUCCEEDED(mkParent) || mkParent == 0x402u)
                    ? fsFsCreateDirectory(&g_fs, kLogDir)
                    : 0xFFFFFFFFu;
            if (R_SUCCEEDED(mk) || mk == 0x402u) {
                g_dir = kLogDir;
            }
            /* The filesystem exists from here on, so the codes of steps 1-3 --
             * which ran before there was anywhere to write -- finally get a
             * home.  Four values in one name: info / current / limit / set. */
            MarkerF("sdv_01_q_%08x_%08x_%08x_%08x", g_diag_rl[0],
                    g_diag_rl[1], g_diag_rl[2], g_diag_rl[3]);
            MarkerF("sdv_02_sm_%08x", g_diag[3]);
            MarkerF("sdv_03_fs_%08x", g_diag[0]);
            MarkerF("sdv_04_sd_%08x", sd);
            /* v565/v570: the folder steps, in their own marker.  0 is "created
             * just now", 402 "already existed", anything else means the files
             * are going to the root.  Two values since logs are nested. */
            MarkerF("sdv_04b_dir_%08x_%08x", mkParent, mk);
            g_phase = 5;
            g_step = 5;
            return;
        }

        case 5: {
            if (!MakePath(g_pathbuf, static_cast<int>(sizeof(g_pathbuf)),
                          kLogName)) {
                g_phase = 0xE5;
                g_failed = true;
                return;
            }
            Marker("sdv_p5");
            /* 0x402 is fs::PathAlreadyExists and is the expected answer from
             * the second launch onward -- the file is kept on purpose so that a
             * crash at the end of it is still readable afterwards.  Only a
             * genuine failure stops the sequence. */
            const unsigned cr = fsFsCreateFile(&g_fs, g_pathbuf, kHdrLen, 0);
            g_diag[2] = cr;
            MarkerF("sdv_05_cr_%08x", cr);
            if (R_FAILED(cr) && cr != 0x402u) {
                g_phase = 0xE5;
                g_failed = true;
                return;
            }
            g_fresh = (cr == 0);
            g_phase = 6;
            g_step = 6;
            return;
        }

        case 6: {
            Marker("sdv_p6");
            FsFile f{};
            unsigned of = fsFsOpenFile(&g_fs, g_pathbuf, FsOpenMode_Write, &f);
            g_of_rc = of;
            MarkerF("sdv_06_op_%08x", of);
            if (R_FAILED(of)) {
                /* Second attempt with Append: if the rejection is about writing
                 * at or past the end of the file, Append is the mode that
                 * permits it, and the marker will say so. */
                of = fsFsOpenFile(&g_fs, g_pathbuf,
                                  FsOpenMode_Write | FsOpenMode_Append, &f);
                MarkerF("sdv_06b_ap_%08x", of);
            }
            if (R_FAILED(of)) {
                g_phase = 0xE6;
                g_failed = true;
                return;
            }
            g_file = f;
            g_phase = 7;
            g_step = 7;
            return;
        }

        case 7: {
            Marker("sdv_p7");
            /* Where does this session start writing?  At the end of what is
             * already there, so the previous session survives.  The size is
             * read from the card rather than remembered, because the counter
             * dies with the process.
             *
             * Unless we just created the file: create(length) allocates that
             * many bytes without clearing them, so getSize would answer with
             * our own request and the header would land after seven bytes of
             * whatever the cluster used to hold.  SDLOG-7 did exactly that and
             * hardware showed it ("S??\0k??" before the tag).  A file we made
             * this session starts at zero by definition. */
            s64 real = 0;
            if (!g_fresh) {
                const unsigned gs = fsFileGetSize(&g_file, &real);
                if (R_FAILED(gs) || real < 0) {
                    real = 0;
                }
            }
            if (real >= kCapBytes) {
                /* Past 2 MB the file is recycled wholesale: delete, create,
                 * reopen.  Delete is the only mutating call hardware has never
                 * refused (every marker file is made with it), whereas rename
                 * answered 0xD401.  Nothing of value is lost -- megabytes of
                 * history are not read line by line, and the tail is what a
                 * crash report needs. */
                fsFileClose(&g_file);
                fsFsDeleteFile(&g_fs, g_pathbuf);
                const unsigned rc = fsFsCreateFile(&g_fs, g_pathbuf, 0, 0);
                FsFile f{};
                const unsigned ro =
                    fsFsOpenFile(&g_fs, g_pathbuf, FsOpenMode_Write, &f);
                MarkerF("sdv_07_rc_%08x_%08x", rc, ro);
                if (R_FAILED(rc) || R_FAILED(ro)) {
                    g_phase = 0xE7;
                    g_failed = true;
                    return;
                }
                g_file = f;
                real = 0;
            }
            g_logBytes = real;
            MarkerF("sdv_07_sz_%08x", static_cast<unsigned>(real));
            const unsigned ss = fsFileSetSize(&g_file, real + kHdrLen);
            MarkerF("sdv_07_ss_%08x", ss);
            if (R_FAILED(ss)) {
                /* Not fatal: the write is worth trying anyway, and it is the
                 * write's result that decides what the file holds. */
            }
            g_phase = 8;
            g_step = 8;
            return;
        }

        case 8: {
            Marker("sdv_p8");
            /* Append the session header at the end of the existing file.  kHdr
             * names the build, so the newest session is always the last such
             * line in the file and older sessions stay underneath it. */
            const unsigned wr =
                fsFileWrite(&g_file, static_cast<u64>(g_logBytes), kHdr,
                            static_cast<u64>(kHdrLen), FsWriteOption_Flush);
            g_wr_rc = wr;
            MarkerF("sdv_08_wr_%08x", wr);
            g_phase = 9;
            g_step = 9;
            return;
        }

        case 9: {
            Marker("sdv_p9");
            const unsigned fl = fsFileFlush(&g_file);
            MarkerF("sdv_09_fl_%08x", fl);
            /* Close is the commit: fsFileClose returns void in libnx, so its
             * result cannot be reported -- the file's size on the card is the
             * only evidence it worked. */
            fsFileClose(&g_file);
            if (R_FAILED(g_wr_rc)) {
                g_phase = 0xE8;
                g_failed = true;
                return;
            }
            Marker("sdv_10_ok");
            g_phase = 0x0A;
            g_wrote = true;
            g_open = false;     /* no handle is held between lines */
            g_logBytes += kHdrLen;
            g_lines = 1;
            g_step = 10;
            return;
        }

        default:
            return;
    }
}

}  // namespace

bool Open() {
    /* Synchronous variant kept for callers that just want it done; it runs the
     * same steps back to back.  Nothing in the HUD uses it -- a blocking call
     * there is what froze a console -- but exl_main and any future caller get
     * the identical code path. */
    while (!g_wrote && !g_failed && g_step < 10) {
        StepOnce();
    }
    return g_wrote;
}

void Step() {
    if (g_wrote || g_failed) {
        return;
    }
    StepOnce();
}

int ReadSetting() {
    char path[64];
    if (!MakePath(path, static_cast<int>(sizeof(path)), "sdv_map.cfg")) {
        return -1;
    }
    char b[2] = {0, 0};
    if (!ReadSmall(path, b, 2)) {
        return -1;
    }
    return (b[0] == '1') ? 1 : 0;
}

bool WriteSetting(int value) {
    char path[64];
    if (!MakePath(path, static_cast<int>(sizeof(path)), "sdv_map.cfg")) {
        return false;
    }
    const char b[2] = {(value != 0) ? '1' : '0', '\n'};
    if (!WriteSmall(path, b, 2)) {
        return false;
    }
    /* Read back: a write that the filesystem accepted but did not persist is
     * indistinguishable from a successful one until the next boot, and by then
     * the player has already lost the setting. */
    char back[2] = {0, 0};
    return ReadSmall(path, back, 2) && back[0] == b[0];
}

int ReadKeepPct() {
    char path[64];
    if (!MakePath(path, static_cast<int>(sizeof(path)), "sdv_keep.cfg")) {
        return -1;
    }
    char b[4] = {0, 0, 0, 0};
    if (!ReadSmall(path, b, 4)) {
        return -1;
    }
    int v = 0;
    for (int i = 0; i < 3; ++i) {
        if (b[i] < '0' || b[i] > '9') {
            return -1;
        }
        v = v * 10 + (b[i] - '0');
    }
    if (v > 100) {
        return -1;
    }
    return v;
}

bool WriteKeepPct(unsigned pct) {
    char path[64];
    if (!MakePath(path, static_cast<int>(sizeof(path)), "sdv_keep.cfg")) {
        return false;
    }
    if (pct > 100u) {
        pct = 100u;
    }
    const char b[4] = {
        static_cast<char>('0' + (pct / 100u) % 10u),
        static_cast<char>('0' + (pct / 10u) % 10u),
        static_cast<char>('0' + pct % 10u),
        '\n'};
    if (!WriteSmall(path, b, 4)) {
        return false;
    }
    char back[4] = {0, 0, 0, 0};
    return ReadSmall(path, back, 4) && back[0] == b[0] && back[1] == b[1] &&
           back[2] == b[2];
}

int Channel() { return g_chan; }
unsigned DiagSm() { return g_diag[3]; }
unsigned DiagInit() { return g_diag[0]; }
unsigned DiagOpenFs() { return g_diag[1]; }
unsigned DiagCreate() { return g_diag[2]; }
unsigned DiagSaveData() { return g_diag[4]; }
unsigned DiagRl() { return g_diag_rl[0]; }
unsigned DiagRlCur() { return g_diag_rl[1]; }
unsigned DiagRlLim() { return g_diag_rl[2]; }
unsigned DiagRlSet() { return g_diag_rl[3]; }
unsigned DiagOpenFile() { return g_of_rc; }
unsigned DiagWrite() { return g_wr_rc; }
unsigned DiagPhase() { return g_phase; }

bool IsOpen() { return g_open || g_wrote; }

/* v647: true once Shutdown() has run.  The caller uses it to keep the fs
 * channel down permanently instead of cycling service sessions. */
bool ShutdownDone() { return g_dead; }

/* Appends one line.  No handle is held between lines: open, extend to the end
 * offset, write with the flush option, close.  Six IPCs per line is the price
 * of never leaving a half-written buffer to be lost by a crash, and the token
 * bucket above keeps a hot path from paying it every frame. */
void LineImpl(const char* fmt, std::va_list ap, bool low, bool force) {
    if (!g_wrote || !g_gate || fmt == nullptr) {
        return;
    }
    if (g_lines >= kCapLines || g_logBytes >= kCapBytes) {
        /* Say so once.  Without this the only symptom is a log that ends
         * mid-session, which reads exactly like a game that stopped logging. */
        if (!g_cap_marked) {
            g_cap_marked = true;
            Marker("sdv_cap");
        }
        return;
    }
    if (!force && !TakeLineSlot(low)) {
        return;
    }

    char buf[256];
    const int n = std::vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n <= 0) {
        return;
    }
    int len = (n < static_cast<int>(sizeof(buf) - 2)) ? n
                                                     : static_cast<int>(sizeof(buf) - 2);
    if (g_logBytes + len + 1 > kCapBytes) {
        return;
    }
    buf[len++] = '\n';

    /* v613: one handle, held across lines.
     *
     * The old path opened, sized, wrote with flush, flushed and closed for
     * every single line -- six IPCs, on the main thread, for a diagnostic
     * channel.  That is the shape of hang this build is investigating, and it
     * is also simply expensive.  The handle is opened once, the size is
     * corrected once, and the flush is deferred so a burst of lines costs two
     * IPCs each instead of six.  On any failure the handle is closed and the
     * next line retries the open, so a session that dies still turns into a
     * failed write rather than a silent one. */
    /* v638: flush every line before Line() returns.  The old 64-line deferred
     * flush made the last line of a dead session unreadable.  v641: SetGate()
     * now closes this handle when the world goes away, so no fs IPC happens
     * during the applet switch that brings up the system keyboard. */
    constexpr unsigned kFlushEvery = 1u;

    if (!s_fileOk) {
        FsFile f{};
        const unsigned of = fsFsOpenFile(&g_fs, g_pathbuf, FsOpenMode_Write, &f);
        if (R_FAILED(of)) {
            NoteWriteFailure(of);
            return;
        }
        s_file = f;
        s_fileOk = true;
        /* The card's own size wins over the counter: a failed append must not
         * leave a hole of NULs in the middle of the log. */
        s64 real = 0;
        if (R_SUCCEEDED(fsFileGetSize(&s_file, &real)) && real >= 0) {
            g_logBytes = real;
        }
    }
    const s64 sz = g_logBytes;
    const unsigned ss = fsFileSetSize(&s_file, sz + static_cast<s64>(len));
    const unsigned wr =
        fsFileWrite(&s_file, sz, buf, static_cast<u64>(len), FsWriteOption_None);
    if (R_FAILED(ss) || R_FAILED(wr)) {
        NoteWriteFailure(R_FAILED(ss) ? ss : wr);
        fsFileClose(&s_file);
        s_fileOk = false;
        return;
    }
    g_logBytes = sz + static_cast<s64>(len);
    ++g_lines;
    if (++s_sinceFlush >= kFlushEvery) {
        s_sinceFlush = 0;
        fsFileFlush(&s_file);
    }
}

/* v613: opened/closed by the update hook.  Off until a location exists.
 *
 * v641: this finally has a caller.  Closing the gate also closes the held
 * handle, so the module holds no fs session across the applet switch. */
void SetGate(bool on) {
    if (g_gate == on) {
        return;
    }
    if (!on) {
        CloseHandle();
    }
    g_gate = on;
}

/* Normal priority: an event worth reading. */
void Line(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    LineImpl(fmt, ap, false, false);
    va_end(ap);
}

/* Low priority: periodic chatter (heartbeats, pass counters).  It still goes to
 * the log, but it is refused whenever the bucket is half spent, so it can never
 * be the reason an event line was dropped. */
void LineLow(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    LineImpl(fmt, ap, true, false);
    va_end(ap);
}

/* v620: the stage marks.  The token bucket's silent drops made "last line
 * before the freeze" ambiguous once -- a frame lost u10 but kept u9, which
 * pointed at the wrong function.  Marks never drop; volume is ~13 short lines
 * per frame only while the trace switch is on. */
void LineForce(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    LineImpl(fmt, ap, false, true);
    va_end(ap);
}

void Close() {
    /* Nothing to close: every line is written and flushed on its own, so a
     * forced power-off loses at most the line in flight rather than the whole
     * session.  Kept for callers that expect a symmetric API. */
    g_open = false;
}

/* v646: see the declaration in sdlog.hpp.  Order matters: the FsFile belongs
 * to the FsFileSystem, which belongs to fsp-srv, which was obtained through
 * sm:.  Close from the inside out. */
void Shutdown() {
    if (g_dead) {
        return;
    }
    g_dead = true;
    CloseHandle();
    if (g_fs_ready) {
        fsFsClose(&g_fs);
        g_fs_ready = false;
    }
    if (g_sm_ok) {
        fsExit();
        smExit();
        g_sm_ok = false;
    }
    /* v647: no reopen.  These resets only make every Line*() a no-op for the
     * rest of the process (g_wrote=false is the important one); EnsureSettings
     * checks ShutdownDone() and never calls Step() again.  The log therefore
     * covers the first world of the process, and a restart starts a new one. */
    g_open = false;
    g_wrote = false;
    g_failed = false;
    g_step = 0;
    g_phase = 0;
    g_chan = 0;
    g_dir = "";
    g_lines = 0;
    g_logBytes = 0;
    g_cap_marked = false;
    g_wr_fail_marked = false;
    g_fresh = false;
    g_gate = true;
    s_fileOk = false;
    s_sinceFlush = 0;
}

}  // namespace sdlog
