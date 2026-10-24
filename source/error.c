/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "util.h"
#include "error.h"

// stderr is redirected to bully.log by the launcher
void fatal_error(const char *fmt, ...) {
  va_list list;

  fprintf(stderr, "FATAL: ");
  va_start(list, fmt);
  vfprintf(stderr, fmt, list);
  va_end(list);
  fprintf(stderr, "\n");
  fflush(stderr);

  exit(1);
}