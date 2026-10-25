/* config.c -- port settings: read config.txt, clamp the values, then rewrite
 *             the file from the commented template with those values
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds, givethesourceplox
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

Config config;

// render size reported to the game; set from the panel by NVEventEGLInit()
int screen_width = 0;
int screen_height = 0;

static void config_defaults(void)
{
  config.render_scale = 1.00f;
  config.clarity = 1;
  config.shadows = 0;
  config.trilinear_filter = 1;
  config.language = 0;
  config.crouch_toggle = 0;
  config.swap_ab_buttons = 0;
  config.swap_xy_buttons = 0;
  config.debug_log = 0;
}

static int clamp_int(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static float clamp_float(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static void config_clamp(void)
{
  config.render_scale = clamp_float(config.render_scale, 0.50f, 1.00f);
  config.clarity = clamp_int(config.clarity, 0, 2);
  config.shadows = clamp_int(config.shadows, 0, 3);
  config.trilinear_filter = clamp_int(config.trilinear_filter, 0, 1);
  config.language = clamp_int(config.language, 0, 6);
  config.crouch_toggle = clamp_int(config.crouch_toggle, 0, 1);
  config.swap_ab_buttons = clamp_int(config.swap_ab_buttons, 0, 1);
  config.swap_xy_buttons = clamp_int(config.swap_xy_buttons, 0, 1);
  config.debug_log = clamp_int(config.debug_log, 0, 1);
}

static void parse_var(const char *name, const char *value)
{
  if (!strcmp(name, "render_scale"))
    config.render_scale = (float)atof(value);
  else if (!strcmp(name, "clarity"))
    config.clarity = atoi(value);
  else if (!strcmp(name, "shadows"))
    config.shadows = atoi(value);
  else if (!strcmp(name, "trilinear_filter"))
    config.trilinear_filter = atoi(value);
  else if (!strcmp(name, "language"))
    config.language = atoi(value);
  else if (!strcmp(name, "crouch_toggle"))
    config.crouch_toggle = atoi(value);
  else if (!strcmp(name, "swap_ab_buttons"))
    config.swap_ab_buttons = atoi(value);
  else if (!strcmp(name, "swap_xy_buttons"))
    config.swap_xy_buttons = atoi(value);
  else if (!strcmp(name, "debug_log"))
    config.debug_log = atoi(value);
  // unknown keys (old files) are ignored and dropped on the next rewrite
}

// defaults, then the user's values from the file (if any), then clamped
int read_config(const char *file)
{
  char line[1024];
  FILE *f;

  config_defaults();

  f = fopen(file, "r");
  if (f == NULL)
  {
    config_clamp();
    return -1;
  }

  while (fgets(line, sizeof(line), f) != NULL)
  {
    char *name = line, *value, *end;

    while (*name && isspace((unsigned char)*name))
      ++name;
    if (*name == '\0' || *name == '#')
      continue;

    for (value = name; *value && !isspace((unsigned char)*value); ++value)
      ;
    if (*value == '\0')
      continue; // key without a value
    *value++ = '\0';

    while (*value && isspace((unsigned char)*value))
      ++value;
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1]))
      *--end = '\0';

    if (*value)
      parse_var(name, value);
  }

  fclose(f);
  config_clamp();
  return 0;
}

// the full commented template with the current values; written to a temp
// file and renamed so a crash mid-write never leaves a broken config.txt
int write_config(const char *file)
{
  char tmp[256];
  FILE *f;

  snprintf(tmp, sizeof(tmp), "%s.tmp", file);
  f = fopen(tmp, "w");
  if (f == NULL)
    return -1;

  fprintf(f,
          "# Bully: Anniversary Edition - port settings\n"
          "#\n"
          "# This file is rewritten from this template every time the game starts.\n"
          "# Your values are kept; anything else you add (extra lines, comments) is not.\n"
          "# Format: one \"key value\" per line. Lines starting with # are ignored.\n"
          "# Out-of-range values are clamped to the nearest valid value.\n"
          "\n"
          "# render_scale\n"
          "#   Fraction of the screen resolution the game renders at. The image is then\n"
          "#   scaled up to the full screen in a single filtered pass.\n"
          "#   Lower is faster but softer. 1.00 renders at full screen resolution with\n"
          "#   no extra scaling pass.\n"
          "#   Valid values: 0.50 to 1.00 (decimals allowed, e.g. 0.60, 0.85)\n"
          "#   Default: 1.00\n"
          "render_scale %.2f\n"
          "\n"
          "# clarity\n"
          "#   The game's own display quality preset: its internal resolution level and\n"
          "#   the amount of post-processing effects. Applies on top of render_scale.\n"
          "#   Higher looks better and costs GPU time.\n"
          "#   Valid values: 0 = Low, 1 = Medium, 2 = High\n"
          "#   Default: 1\n"
          "clarity %d\n"
          "\n"
          "# shadows\n"
          "#   Shadow quality. Shadows are one of the most expensive features on this GPU.\n"
          "#   If the game reports a lower maximum for this device, the value is capped to it.\n"
          "#   Valid values: 0 = Off, 1 = Low, 2 = Medium, 3 = High\n"
          "#   Default: 0\n"
          "shadows %d\n"
          "\n"
          "# trilinear_filter\n"
          "#   Smooths the transition between texture detail levels (removes visible\n"
          "#   \"bands\" on floors and roads). Small GPU cost.\n"
          "#   Valid values: 0 = off (game default), 1 = on\n"
          "#   Default: 1\n"
          "trilinear_filter %d\n"
          "\n"
          "# language\n"
          "#   Language of the game's text (menus, subtitles, mission text).\n"
          "#   Valid values: 0 = English, 1 = Spanish, 2 = French, 3 = German,\n"
          "#                 4 = Italian, 5 = Japanese, 6 = Russian\n"
          "#   Default: 0\n"
          "language %d\n"
          "\n"
          "# crouch_toggle\n"
          "#   How the crouch button (R3, right stick click) works.\n"
          "#   Valid values: 0 = hold R3 to stay crouched (game default)\n"
          "#                 1 = press R3 once to crouch, press again to stand\n"
          "#   Default: 0\n"
          "crouch_toggle %d\n"
          "\n"
          "# swap_ab_buttons\n"
          "#   Swaps the actions of the bottom and right face buttons. Use it if\n"
          "#   confirm and back feel reversed on your device.\n"
          "#   Valid values: 0 = off, 1 = swapped\n"
          "#   Default: 0\n"
          "swap_ab_buttons %d\n"
          "\n"
          "# swap_xy_buttons\n"
          "#   Swaps the actions of the left and top face buttons.\n"
          "#   Valid values: 0 = off, 1 = swapped\n"
          "#   Default: 0\n"
          "swap_xy_buttons %d\n"
          "\n"
          "# debug_log\n"
          "#   Writes detailed technical tracing (graphics calls, file access, thread\n"
          "#   hand-offs) to bully.log. Only useful when tracking down a bug: the log\n"
          "#   grows by thousands of lines per run.\n"
          "#   With 0 the log only holds setup, display, exit and error messages.\n"
          "#   Valid values: 0 = off, 1 = on\n"
          "#   Default: 0\n"
          "debug_log %d\n",
          config.render_scale, config.clarity, config.shadows, config.trilinear_filter,
          config.language, config.crouch_toggle,
          config.swap_ab_buttons, config.swap_xy_buttons, config.debug_log);

  if (fclose(f) != 0 || rename(tmp, file) != 0)
  {
    remove(tmp);
    return -1;
  }
  return 0;
}