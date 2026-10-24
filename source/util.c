/* util.c -- misc utility functions
 *
 * Copyright (C) 2026 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "util.h"
#include "config.h"

static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_main_thread;
static int s_main_thread_set = 0;
static int s_compat_delay_ms = 2;

void debugPrintf_setMainThread(void)
{
  s_main_thread = pthread_self();
  s_main_thread_set = 1;
}

void debugPrintf_setCompatDelayMs(int ms)
{
  if (ms < 0)
    ms = 0;
  if (ms > 20)
    ms = 20;
  s_compat_delay_ms = ms;
}

// Verbose tracing: printed only with debug_log 1 in config.txt. The lock and
// the stall-delay workaround apply either way, since the Switch build's
// thread timing depended on them.
int debugPrintf(const char *text, ...)
{
  va_list list;
  const char *tag = "?";

  if (s_main_thread_set)
    tag = pthread_equal(pthread_self(), s_main_thread) ? "M" : "G";

  pthread_mutex_lock(&s_log_mutex);

  if (config.debug_log)
  {
    printf("[%s] ", tag);
    va_start(list, text);
    vprintf(text, list);
    va_end(list);
  }

  if (s_compat_delay_ms > 0)
  {
    struct timespec ts = {0, (long)s_compat_delay_ms * 1000000L};
    nanosleep(&ts, NULL);
  }

  pthread_mutex_unlock(&s_log_mutex);
  return 0;
}

// Always printed: setup, display, exit and error messages
int logPrintf(const char *text, ...)
{
  va_list list;

  pthread_mutex_lock(&s_log_mutex);
  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  pthread_mutex_unlock(&s_log_mutex);
  return 0;
}

int ret0(void) { return 0; }

int ret1(void) { return 1; }

int retm1(void) { return -1; }
