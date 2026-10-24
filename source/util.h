/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>
#include <time.h>

int debugPrintf(const char *text, ...);
int logPrintf(const char *text, ...);
void debugPrintf_setMainThread(void);

int ret0(void);
int ret1(void);
int retm1(void);

static inline uint64_t umin(uint64_t a, uint64_t b)
{
  return (a < b) ? a : b;
}

static inline uint64_t mono_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void sleep_ns(uint64_t ns)
{
  struct timespec ts = {(time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL)};
  nanosleep(&ts, NULL);
}

#endif
