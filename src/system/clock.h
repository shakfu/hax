/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_CLOCK_H
#define HAX_SYSTEM_CLOCK_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CLOCK_MONOTONIC milliseconds since an unspecified epoch. */
long monotonic_ms(void);

/* The CLOCK_REALTIME time `ms` from now, as pthread_cond_timedwait takes it: that is the clock it
 * portably uses, and macOS cannot select CLOCK_MONOTONIC for it. */
struct timespec realtime_deadline_ms(long ms);

#ifdef __cplusplus
}
#endif

#endif /* HAX_SYSTEM_CLOCK_H */
