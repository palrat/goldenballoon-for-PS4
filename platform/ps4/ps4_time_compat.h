/* ps4_time_compat.h — force-included.
 *
 * The Orbis libc offers nanosleep but neither CLOCK_MONOTONIC nor
 * clock_gettime in <time.h>; libkernel's process clock is the monotonic source.
 * Four engine files call clock_gettime(CLOCK_MONOTONIC, ...) for frame pacing
 * and profiling, so the call is provided rather than those files being edited.
 */
#ifndef PS4_TIME_COMPAT_H
#define PS4_TIME_COMPAT_H

#include <time.h>
#include <orbis/libkernel.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

static inline int ps4_clock_gettime(int clk_id, struct timespec *tp) {
    (void)clk_id;
    if (tp == 0) return -1;
    uint64_t us = sceKernelGetProcessTime();
    tp->tv_sec = (time_t)(us / 1000000ull);
    tp->tv_nsec = (long)((us % 1000000ull) * 1000ull);
    return 0;
}
#define clock_gettime(clk, ts) ps4_clock_gettime((clk), (ts))

#endif
