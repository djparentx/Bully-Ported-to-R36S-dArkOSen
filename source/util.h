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

int debugPrintf(const char *text, ...);
void debugPrintf_setMainThread(void);
void debugPrintf_setCompatDelayMs(int ms);

int ret0(void);
int ret1(void);
int retm1(void);

static inline uint64_t umin(uint64_t a, uint64_t b)
{
  return (a < b) ? a : b;
}

#endif
