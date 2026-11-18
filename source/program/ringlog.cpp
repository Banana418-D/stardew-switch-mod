#include "program/ringlog.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ringlog {

/* Defined without an initialiser on purpose: the whole 128 KB lands in .bss,
 * so subsdk8 does not grow by 128 KB.  Init() fills in the header. */
Ring g_ring;

void Init(int build) {
    /* .bss is already zero; only the header needs writing.  Do not clear
     * data[] here -- a warm start could otherwise destroy a previous ring the
     * overlay has not exported yet. */
    std::memcpy(g_ring.magic, kMagic, sizeof(kMagic));
    __atomic_store_n(&g_ring.cap, kCap, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ring.build, static_cast<uint32_t>(build), __ATOMIC_RELEASE);
    __atomic_store_n(&g_ring.head, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ring.seq, 0u, __ATOMIC_RELEASE);
}

void Append(const char* fmt, ...) {
    if (fmt == nullptr) {
        return;
    }
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    int len = (n < static_cast<int>(sizeof(line) - 2))
                  ? n
                  : static_cast<int>(sizeof(line) - 2);
    line[len++] = '\n';

    /* Single writer: every log call comes from a hook on the game's main
     * thread.  The two counters are published with atomics so the overlay's
     * asynchronous snapshot can never see a head that is newer than the
     * bytes it is about to read.  A torn tail (half a line) is acceptable --
     * the report reads the tail, and the next export completes it. */
    uint32_t h = __atomic_load_n(&g_ring.head, __ATOMIC_ACQUIRE);
    if (h >= kCap) {
        h = 0;
    }
    for (int i = 0; i < len; ++i) {
        g_ring.data[h++] = line[i];
        if (h == kCap) {
            h = 0;
        }
    }
    __atomic_store_n(&g_ring.head, h, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ring.seq, static_cast<uint32_t>(len), __ATOMIC_RELEASE);
}

}  // namespace ringlog
