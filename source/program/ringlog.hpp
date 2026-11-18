#pragma once

/* v648: the mod's log lives in RAM, not on the SD card.
 *
 * Why: smInitialize()/fsInitialize()/fsOpenSdCardFileSystem() open three
 * service sessions that used to be held for the whole process.  Holding them
 * across the system-keyboard applet switch hangs the main thread (v645 proved
 * it), and cycling them on every world transition preceded a flash-exit
 * (v646).  A mod that never touches fs cannot do either.
 *
 * The Tesla overlay is a SEPARATE process with its own SD-card channel.  It
 * reads this ring through dmnt:cht and appends new bytes to
 * sdv_native/logs/sdv_modlog.txt.  See docs/RINGLOG_PLAN.md.
 *
 * v649: the capacity changed, so the magic version was bumped to -02.  An
 * older overlay looking for -01 simply finds nothing instead of misreading
 * the header -- fail closed, never silently corrupt.
 *
 * The overlay has no symbol table, so it finds this struct by scanning the
 * injected module for kMagic -- the same trick settime::Request already
 * describes in set_time.hpp.  Nothing about the address is baked into the
 * .ovl, so one overlay build works with every mod build. */

#include <cstdint>

namespace ringlog {

/* v649: 1 MiB.  Steady-state logging is ~13 B/s (heartbeat every 256
 * frames + gate every 300 frames), so 128 KiB covered only ~2.8 hours and
 * 1 MiB covers roughly a full day of play.  It lives in .bss, so subsdk8's
 * file size does not grow; only the game process's RAM does (by 1 MiB). */
constexpr uint32_t kCap = 1024u * 1024u;
/* 14 characters + NUL + one pad byte = exactly 16.  Changing this breaks the
 * overlay's scan, which is why it carries a version suffix. */
constexpr char kMagic[16] = "SDV-LOGRING-02";

struct Ring {
    char     magic[16];
    uint32_t seq;              /* total bytes ever written, monotonic */
    uint32_t cap;              /* kCap, for the reader to sanity-check */
    uint32_t head;             /* next write position in data[] */
    uint32_t build;            /* kBuildNo, so a stale ring is detectable */
    char     data[kCap];       /* plain text, one line per '\n' */
};

/* Zero-initialised -> .bss.  It costs RAM but no space in subsdk8. */
extern Ring g_ring;

void Init(int build);
void Append(const char* fmt, ...);

}  // namespace ringlog
