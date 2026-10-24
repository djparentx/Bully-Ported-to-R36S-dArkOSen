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
static uint64_t s_log_start_ns;

// clock for the log's time column: starts before main()
__attribute__((constructor)) static void log_clock_start(void)
{
  s_log_start_ns = mono_ns();
}

static double log_seconds(void)
{
  return (double)(mono_ns() - s_log_start_ns) / 1e9;
}

void debugPrintf_setMainThread(void)
{
  s_main_thread = pthread_self();
  s_main_thread_set = 1;
}

// Verbose tracing: printed only with debug_log 1 in config.txt; otherwise
// returns at once (no lock, no delay)
int debugPrintf(const char *text, ...)
{
  va_list list;
  const char *tag = "?";

  if (!config.debug_log)
    return 0;

  if (s_main_thread_set)
    tag = pthread_equal(pthread_self(), s_main_thread) ? "M" : "G";

  pthread_mutex_lock(&s_log_mutex);
  printf("[%7.2f] [%s] ", log_seconds(), tag);
  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  fflush(stdout);
  pthread_mutex_unlock(&s_log_mutex);
  return 0;
}

// Always printed: setup, display, exit and error messages
int logPrintf(const char *text, ...)
{
  va_list list;

  pthread_mutex_lock(&s_log_mutex);
  printf("[%7.2f] ", log_seconds());
  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  fflush(stdout); // on disk now: an out-of-memory kill drops anything still buffered
  pthread_mutex_unlock(&s_log_mutex);
  return 0;
}

int ret0(void) { return 0; }

int ret1(void) { return 1; }

int retm1(void) { return -1; }
