/* setup.c -- first-run installer (extracts the game from the user's own APK)
 *            and the loading splash
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <SDL2/SDL.h>
#include <minizip/unzip.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "stb_easy_font.h"

#include "config.h"
#include "hooks.h"
#include "util.h"
#include "setup.h"

#define APK_LIB_GAME   "lib/arm64-v8a/libGame.so"
#define APK_LIB_CPP    "lib/arm64-v8a/libc++_shared.so"
#define APK_BANNER     "res/drawable/banner.png"
#define APK_BG         "assets/flutter_assets/assets/images/bg.horizontal.png" // same path once installed
#define CPP_LIB_NAME   "libc++_shared.so"
#define BANNER_NAME    "banner.png"
#define INSTALL_MARKER ".installing"
#define UI_FONT        "/usr/bin/emulationstation/resources/OpenSans-CondLight.ttf"
#define EXTRACT_BUF    (1 << 20)
#define SPACE_MARGIN   (64ULL << 20)
#define UI_POLL_MS     30
#define UI_REDRAW_MS   100

enum
{
  ST_NO_APK,
  ST_READY,
  ST_INSTALLING,
  ST_DONE,
  ST_FAILED
};

static volatile int s_state = ST_NO_APK;
static char s_apk[256];
static char s_msg[96];
static char s_current[96];
static unsigned long long s_total, s_done, s_free;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

// UI: background texture + a panel-sized RGBA overlay (text, bar) blended on top
static int s_pw, s_ph;
static uint8_t *s_ov;
static unsigned int s_ov_tex, s_bg_tex;
static int s_bg_w, s_bg_h;

static stbtt_fontinfo s_font;
static unsigned char *s_font_data;
static int s_have_font;

static const uint8_t COL_GOLD[3] = {0xf2, 0xc1, 0x4e};
static const uint8_t COL_TRACK[3] = {0x14, 0x22, 0x3a};
static const uint8_t COL_BLUE[3] = {0x2f, 0x7f, 0xe8};

// ---------------------------------------------------------------------------
// files and the APK
// ---------------------------------------------------------------------------

static int file_ok(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && st.st_size > 0;
}

int setup_is_installed(void)
{
  if (access(INSTALL_MARKER, F_OK) == 0)
    return 0; // an earlier install was interrupted
  return file_ok(SO_NAME) && file_ok(CPP_LIB_NAME) && file_ok("assets/data_0.zip");
}

static void set_failed(const char *fmt, const char *arg)
{
  snprintf(s_msg, sizeof(s_msg), fmt, arg ? arg : "");
  logPrintf("setup: FAILED: %s\n", s_msg);
  __atomic_store_n(&s_state, ST_FAILED, __ATOMIC_RELEASE);
}

// APK entry -> destination path in the game folder, or NULL to skip it
static const char *dest_for(const char *name, char *buf, size_t size)
{
  size_t n = strlen(name);

  if (strcmp(name, APK_LIB_GAME) == 0)
    return SO_NAME;
  if (strcmp(name, APK_LIB_CPP) == 0)
    return CPP_LIB_NAME;
  if (strcmp(name, APK_BANNER) == 0)
    return BANNER_NAME;
  if (strncmp(name, "assets/", 7) != 0 || name[n - 1] == '/' || strstr(name, ".."))
    return NULL;
  snprintf(buf, size, "%s", name);
  return buf;
}

static int make_parents(const char *path)
{
  char tmp[PATH_MAX];
  char *p;

  snprintf(tmp, sizeof(tmp), "%s", path);
  for (p = tmp + 1; *p; p++)
  {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
      return -1;
    *p = '/';
  }
  return 0;
}

static int find_apk(char *out, size_t size)
{
  DIR *d = opendir(".");
  struct dirent *e;
  int found = 0;

  if (!d)
    return 0;
  while ((e = readdir(d)) != NULL)
  {
    size_t n = strlen(e->d_name);
    if (n > 4 && strcasecmp(e->d_name + n - 4, ".apk") == 0)
    {
      snprintf(out, size, "%s", e->d_name);
      found = 1;
      break;
    }
  }
  closedir(d);
  return found;
}

// one APK entry into memory (caller frees)
static unsigned char *apk_read_entry(const char *apk, const char *entry, size_t *size)
{
  unzFile zf = unzOpen64(apk);
  unz_file_info64 info;
  unsigned char *buf = NULL;

  if (!zf)
    return NULL;
  if (unzLocateFile(zf, entry, 1) == UNZ_OK &&
      unzGetCurrentFileInfo64(zf, &info, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK &&
      unzOpenCurrentFile(zf) == UNZ_OK)
  {
    buf = malloc(info.uncompressed_size ? (size_t)info.uncompressed_size : 1);
    if (buf && unzReadCurrentFile(zf, buf, (unsigned)info.uncompressed_size) != (int)info.uncompressed_size)
    {
      free(buf);
      buf = NULL;
    }
    if (unzCloseCurrentFile(zf) != UNZ_OK)
    {
      free(buf);
      buf = NULL;
    }
    if (buf)
      *size = (size_t)info.uncompressed_size;
  }
  unzClose(zf);
  return buf;
}

// RGBA image from the game folder, or straight from the APK if not extracted yet
static unsigned char *image_load(const char *path, const char *apk_entry, int *w, int *h)
{
  int n;
  unsigned char *img = stbi_load(path, w, h, &n, 4);

  if (!img && s_apk[0])
  {
    size_t size = 0;
    unsigned char *data = apk_read_entry(s_apk, apk_entry, &size);
    if (data)
    {
      img = stbi_load_from_memory(data, (int)size, w, h, &n, 4);
      free(data);
    }
  }
  if (!img)
    logPrintf("setup: image %s not available\n", path);
  return img;
}

// banner.png is needed by the loading screen; installs made before it was
// part of the install get just this one file from the APK
static void banner_ensure(void)
{
  size_t size = 0;
  unsigned char *data;
  FILE *f;

  if (file_ok(BANNER_NAME) || !s_apk[0])
    return;
  data = apk_read_entry(s_apk, APK_BANNER, &size);
  if (!data)
    return;
  f = fopen(BANNER_NAME, "wb");
  if (f)
  {
    int ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0 || !ok)
      remove(BANNER_NAME);
    else
      logPrintf("setup: extracted %s\n", BANNER_NAME);
  }
  free(data);
}

// checks the APK holds what we need and that it fits on the card
static int apk_scan(void)
{
  unzFile zf = unzOpen64(s_apk);
  int has_game = 0, has_cpp = 0, assets = 0, err;
  struct statvfs vfs;

  if (!zf)
  {
    set_failed("Cannot open %.40s", s_apk);
    return 0;
  }

  s_total = 0;
  for (err = unzGoToFirstFile(zf); err == UNZ_OK; err = unzGoToNextFile(zf))
  {
    unz_file_info64 info;
    char name[512], buf[512];

    if (unzGetCurrentFileInfo64(zf, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
      continue;
    if (!dest_for(name, buf, sizeof(buf)))
      continue;
    s_total += info.uncompressed_size;
    if (strcmp(name, APK_LIB_GAME) == 0)
      has_game = 1;
    else if (strcmp(name, APK_LIB_CPP) == 0)
      has_cpp = 1;
    else if (strcmp(name, APK_BANNER) != 0)
      assets++;
  }
  unzClose(zf);

  logPrintf("setup: %s: %d asset files, %llu MB, libGame.so %s, libc++_shared.so %s\n",
            s_apk, assets, s_total >> 20, has_game ? "yes" : "no", has_cpp ? "yes" : "no");

  if (!has_game || !has_cpp)
  {
    set_failed("%s", "No arm64 libs (need APK v1.4.311 arm64)");
    return 0;
  }
  if (!assets)
  {
    set_failed("%s", "No assets/ inside this APK");
    return 0;
  }

  if (statvfs(".", &vfs) == 0)
  {
    s_free = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    if (s_free < s_total + SPACE_MARGIN)
    {
      char need[32];
      snprintf(need, sizeof(need), "%llu MB", (s_total + SPACE_MARGIN) >> 20);
      set_failed("Not enough space: need %s free", need);
      return 0;
    }
  }

  return 1;
}

static void *install_thread(void *arg)
{
  unzFile zf;
  char *buf = malloc(EXTRACT_BUF);
  int err, last_tenth = -1;

  (void)arg;

  zf = unzOpen64(s_apk);
  if (!zf || !buf)
  {
    set_failed("Cannot open %.40s", s_apk);
    goto out;
  }

  for (err = unzGoToFirstFile(zf); err == UNZ_OK; err = unzGoToNextFile(zf))
  {
    unz_file_info64 info;
    char name[512], path[512];
    const char *dest;
    FILE *f;
    int n, write_ok = 1;

    if (unzGetCurrentFileInfo64(zf, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
    {
      set_failed("%s", "Bad APK directory");
      goto out;
    }
    dest = dest_for(name, path, sizeof(path));
    if (!dest)
      continue;

    pthread_mutex_lock(&s_lock);
    snprintf(s_current, sizeof(s_current), "%.95s", name); // cut to fit the 96-byte status line
    pthread_mutex_unlock(&s_lock);

    if (make_parents(dest) < 0)
    {
      set_failed("Cannot create folder: %.40s", dest);
      goto out;
    }
    if (unzOpenCurrentFile(zf) != UNZ_OK)
    {
      set_failed("Cannot read: %.40s", name);
      goto out;
    }
    f = fopen(dest, "wb");
    if (!f)
    {
      unzCloseCurrentFile(zf);
      set_failed("Cannot write: %.40s", dest);
      goto out;
    }

    while ((n = unzReadCurrentFile(zf, buf, EXTRACT_BUF)) > 0)
    {
      if (fwrite(buf, 1, (size_t)n, f) != (size_t)n)
      {
        write_ok = 0;
        break;
      }
      pthread_mutex_lock(&s_lock);
      s_done += (unsigned long long)n;
      pthread_mutex_unlock(&s_lock);
    }
    if (fclose(f) != 0)
      write_ok = 0;
    if (unzCloseCurrentFile(zf) != UNZ_OK) // CRC mismatch
      n = -1;

    if (!write_ok)
    {
      set_failed("Cannot write: %.40s", dest);
      goto out;
    }
    if (n < 0)
    {
      set_failed("Extract failed: %.40s", name);
      goto out;
    }

    if (s_total)
    {
      int tenth = (int)(s_done * 10 / s_total);
      if (tenth != last_tenth)
      {
        logPrintf("setup: %3d%%  %s\n", tenth * 10, name);
        last_tenth = tenth;
      }
    }
  }

  if (err != UNZ_END_OF_LIST_OF_FILE)
  {
    set_failed("%s", "Bad APK directory");
    goto out;
  }

  sync();
  unlink(INSTALL_MARKER);
  logPrintf("setup: install complete\n");
  __atomic_store_n(&s_state, ST_DONE, __ATOMIC_RELEASE);

out:
  if (zf)
    unzClose(zf);
  free(buf);
  return NULL;
}

// ---------------------------------------------------------------------------
// overlay drawing (straight-alpha RGBA, panel size)
// ---------------------------------------------------------------------------

static void ov_blend(int x, int y, const uint8_t c[3], int a)
{
  uint8_t *p;
  int da, oa;

  if (x < 0 || y < 0 || x >= s_pw || y >= s_ph || a <= 0)
    return;
  p = s_ov + ((size_t)y * s_pw + x) * 4;
  da = p[3];
  oa = a + da * (255 - a) / 255;
  for (int i = 0; i < 3; i++)
    p[i] = (uint8_t)((c[i] * a + p[i] * da * (255 - a) / 255) / oa);
  p[3] = (uint8_t)oa;
}

static void ov_rect(int x, int y, int w, int h, const uint8_t c[3], int a)
{
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      ov_blend(xx, yy, c, a);
}

static void font_load(void)
{
  FILE *f;
  long n;

  if (s_have_font)
    return;
  f = fopen(UI_FONT, "rb");
  if (!f)
  {
    logPrintf("setup: %s missing, using the block font\n", UI_FONT);
    return;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  s_font_data = n > 0 ? malloc((size_t)n) : NULL;
  if (s_font_data && fread(s_font_data, 1, (size_t)n, f) == (size_t)n &&
      stbtt_InitFont(&s_font, s_font_data, stbtt_GetFontOffsetForIndex(s_font_data, 0)))
  {
    s_have_font = 1;
  }
  else
  {
    logPrintf("setup: cannot read %s, using the block font\n", UI_FONT);
    free(s_font_data);
    s_font_data = NULL;
  }
  fclose(f);
}

static int block_scale(int px)
{
  int k = px / 12;
  return k < 1 ? 1 : k;
}

static int text_width(const char *s, int px)
{
  if (s_have_font)
  {
    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    float w = 0.0f;

    for (const char *p = s; *p; p++)
    {
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&s_font, (unsigned char)p[0], &adv, &lsb);
      w += adv * scale;
      if (p[1])
        w += stbtt_GetCodepointKernAdvance(&s_font, (unsigned char)p[0], (unsigned char)p[1]) * scale;
    }
    return (int)(w + 0.5f);
  }
  return stb_easy_font_width((char *)s) * block_scale(px);
}

// text with its top-left at (x, y), px = line height in pixels
static void text_draw(int x, int y, int px, const char *s, const uint8_t c[3])
{
  if (s_have_font)
  {
    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    float pen = (float)x;
    int ascent, descent, gap, base;

    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &gap);
    base = y + (int)(ascent * scale);

    for (const char *p = s; *p; p++)
    {
      int cp = (unsigned char)p[0];
      int adv, lsb, bw, bh, xo, yo;
      float px0 = floorf(pen);
      unsigned char *bm = stbtt_GetCodepointBitmapSubpixel(&s_font, scale, scale, pen - px0, 0.0f,
                                                           cp, &bw, &bh, &xo, &yo);
      if (bm)
      {
        for (int yy = 0; yy < bh; yy++)
          for (int xx = 0; xx < bw; xx++)
            ov_blend((int)px0 + xo + xx, base + yo + yy, c, bm[yy * bw + xx]);
        stbtt_FreeBitmap(bm, NULL);
      }
      stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
      pen += adv * scale;
      if (p[1])
        pen += stbtt_GetCodepointKernAdvance(&s_font, cp, (unsigned char)p[1]) * scale;
    }
    return;
  }

  // block font fallback: stb_easy_font's axis-aligned quads, scaled up
  {
    static char vbuf[64 * 4096];
    char text[256];
    int k = block_scale(px);
    int quads;

    snprintf(text, sizeof(text), "%s", s);
    quads = stb_easy_font_print(0.0f, 0.0f, text, NULL, vbuf, sizeof(vbuf));
    for (int q = 0; q < quads; q++)
    {
      const float *v0 = (const float *)(vbuf + q * 64);
      const float *v2 = (const float *)(vbuf + q * 64 + 32);
      int x0 = (int)v0[0], y0 = (int)v0[1];
      ov_rect(x + x0 * k, y + y0 * k, ((int)v2[0] - x0) * k, ((int)v2[1] - y0) * k, c, 255);
    }
  }
}

static void draw_fill(unsigned int tex, int iw, int ih)
{
  float sx = (float)s_pw / (float)iw;
  float sy = (float)s_ph / (float)ih;
  float s = sx > sy ? sx : sy;              // fill the screen
  float us = (float)s_pw / ((float)iw * s); // visible fraction (centered crop)
  float vs = (float)s_ph / ((float)ih * s);
  float u0 = (1.0f - us) * 0.5f;
  float v0 = (1.0f - vs) * 0.5f;

  video_draw_tex(tex, 0.0f, 0.0f, (float)s_pw, (float)s_ph, u0, v0, u0 + us, v0 + vs, 0);
}

// EmulationStation's InvertButtons swaps A/B for everything, PortMaster's
// mapping included: the button logic stays, only the labels follow it
static char s_btn_ok = 'A', s_btn_back = 'B';

static int es_buttons_inverted(void)
{
  char path[PATH_MAX], line[256];
  const char *home = getenv("HOME");
  FILE *f;
  int inverted = 0;

  snprintf(path, sizeof(path), "%s/.emulationstation/es_settings.cfg",
           home && home[0] ? home : "/home/ark");
  f = fopen(path, "r");
  if (!f)
    return 0;
  while (fgets(line, sizeof(line), f) != NULL)
  {
    if (strstr(line, "name=\"InvertButtons\""))
    {
      inverted = strstr(line, "value=\"true\"") != NULL;
      break;
    }
  }
  fclose(f);
  return inverted;
}

// ---------------------------------------------------------------------------
// installer screen
// ---------------------------------------------------------------------------

typedef struct
{
  char text[128];
  int px;  // text height, or bar height
  int bar; // 1 = progress bar line
} UiLine;

static int ui_block_height(const UiLine *lines, int n, int gap)
{
  int h = gap; // extra space after the title

  for (int i = 0; i < n; i++)
    h += lines[i].bar ? lines[i].px + 2 * gap : lines[i].px * 4 / 3;
  return h;
}

static void ui_layout_draw(void)
{
  UiLine lines[8];
  int n = 0, pct = 0, total_h, y;
  int max_w = s_pw * 9 / 10;
  int max_h = s_ph * 9 / 10;
  int title_px = s_ph * 2 / 13;
  int body_px = s_ph / 11;
  int bar_w = s_pw * 6 / 10;
  int bar_h = s_ph / 15;
  int gap = body_px / 2;
  int state = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);

#define UI_TEXT(px_, ...)                                                 \
  do                                                                      \
  {                                                                       \
    snprintf(lines[n].text, sizeof(lines[n].text), __VA_ARGS__);          \
    lines[n].px = (px_);                                                  \
    lines[n].bar = 0;                                                     \
    n++;                                                                  \
  } while (0)

  UI_TEXT(title_px, "%s", "Bully: Anniversary Edition");

  switch (state)
  {
  case ST_NO_APK:
    UI_TEXT(body_px, "%s", "Game data not installed.");
    UI_TEXT(body_px, "%s", "Copy your Bully APK (v1.4.311, arm64) into ports/bully,");
    UI_TEXT(body_px, "%s", "then start the game again.");
    UI_TEXT(body_px, "%c: exit", s_btn_back);
    break;

  case ST_READY:
    UI_TEXT(body_px, "APK: %.50s", s_apk);
    UI_TEXT(body_px, "Needs %llu MB, %llu MB free", s_total >> 20, s_free >> 20);
    UI_TEXT(body_px, "%c: install      %c: exit", s_btn_ok, s_btn_back);
    break;

  case ST_INSTALLING:
  {
    unsigned long long done, total;
    char current[96];

    pthread_mutex_lock(&s_lock);
    done = s_done;
    total = s_total;
    snprintf(current, sizeof(current), "%s", s_current);
    pthread_mutex_unlock(&s_lock);
    pct = total ? (int)(done * 100 / total) : 0;

    UI_TEXT(body_px, "%s", "Installing... please wait");
    lines[n].text[0] = '\0';
    lines[n].px = bar_h;
    lines[n].bar = 1;
    n++;
    UI_TEXT(body_px, "%d%%   %.50s", pct, current);
    break;
  }

  case ST_DONE:
    UI_TEXT(body_px, "%s", "Installed.");
    UI_TEXT(body_px, "%c: launch      %c: exit", s_btn_ok, s_btn_back);
    break;

  default:
    UI_TEXT(body_px, "%s", "Setup failed:");
    UI_TEXT(body_px, "%.60s", s_msg);
    UI_TEXT(body_px, "%c: exit", s_btn_back);
    break;
  }
#undef UI_TEXT

  // each line no wider than 90% of the screen
  for (int i = 0; i < n; i++)
  {
    int w;

    if (lines[i].bar)
      continue;
    w = text_width(lines[i].text, lines[i].px);
    if (w > max_w)
      lines[i].px = lines[i].px * max_w / w;
  }

  // the whole block no taller than 90% of the screen
  total_h = ui_block_height(lines, n, gap);
  if (total_h > max_h)
  {
    for (int i = 0; i < n; i++)
      lines[i].px = lines[i].px * max_h / total_h;
    gap = gap * max_h / total_h;
    total_h = ui_block_height(lines, n, gap);
  }

  y = (s_ph - total_h) / 2;

  for (int i = 0; i < n; i++)
  {
    if (lines[i].bar)
    {
      int h = lines[i].px;
      int x = (s_pw - bar_w) / 2;
      int inset = h / 6 > 1 ? h / 6 : 1;
      int fill = (bar_w - 2 * inset) * pct / 100;

      y += gap;
      ov_rect(x, y, bar_w, h, COL_TRACK, 210);
      ov_rect(x + inset, y + inset, fill, h - 2 * inset, COL_BLUE, 255);
      y += h + gap;
      continue;
    }
    text_draw((s_pw - text_width(lines[i].text, lines[i].px)) / 2, y, lines[i].px, lines[i].text, COL_GOLD);
    y += lines[i].px * 4 / 3;
    if (i == 0)
      y += gap;
  }
}

static void ui_frame(void)
{
  memset(s_ov, 0, (size_t)s_pw * s_ph * 4);
  ui_layout_draw();

  video_clear();
  if (s_bg_tex)
    draw_fill(s_bg_tex, s_bg_w, s_bg_h);
  video_tex_update(s_ov_tex, s_ov, s_pw, s_ph);
  video_draw_tex(s_ov_tex, 0.0f, 0.0f, (float)s_pw, (float)s_ph, 0.0f, 0.0f, 1.0f, 1.0f, 1);
  video_present();
}

static int ui_init(void)
{
  unsigned char *img;
  int w, h;

  video_panel_size(&s_pw, &s_ph);
  if (s_pw <= 0 || s_ph <= 0)
    return 0;
  s_ov = calloc((size_t)s_pw * s_ph, 4);
  if (!s_ov)
    return 0;
  s_ov_tex = video_tex_create(s_ov, s_pw, s_ph);
  font_load();

  s_btn_ok = es_buttons_inverted() ? 'B' : 'A';
  s_btn_back = s_btn_ok == 'A' ? 'B' : 'A';
  logPrintf("setup: accept button labelled %c\n", s_btn_ok);

  img = image_load(APK_BG, APK_BG, &w, &h);
  if (img)
  {
    s_bg_tex = video_tex_create(img, w, h);
    s_bg_w = w;
    s_bg_h = h;
    stbi_image_free(img);
  }
  return 1;
}

static void ui_shutdown(void)
{
  if (s_ov_tex)
    video_tex_free(s_ov_tex);
  if (s_bg_tex)
    video_tex_free(s_bg_tex);
  s_ov_tex = s_bg_tex = 0;
  free(s_ov);
  s_ov = NULL;
  free(s_font_data);
  s_font_data = NULL;
  s_have_font = 0;
}

static SDL_GameController *setup_open_pad(void)
{
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
  {
    logPrintf("setup: controller init failed: %s\n", SDL_GetError());
    return NULL;
  }
  // state is polled; nothing drains the event queue
  SDL_GameControllerEventState(SDL_IGNORE);
  SDL_JoystickEventState(SDL_IGNORE);

  for (int i = 0; i < SDL_NumJoysticks(); i++)
  {
    if (!SDL_IsGameController(i))
      continue;
    SDL_GameController *pad = SDL_GameControllerOpen(i);
    if (pad)
    {
      logPrintf("setup: controller %s\n", SDL_GameControllerName(pad));
      return pad;
    }
  }
  logPrintf("setup: no game controller found\n");
  return NULL;
}

int setup_run(void)
{
  SDL_GameController *pad;
  pthread_t thread;
  int thread_started = 0;
  int prev_a = 1, prev_b = 1; // a button must be released before it counts
  int result = -1, last_state = -1;
  Uint32 last_draw = 0;

  if (!find_apk(s_apk, sizeof(s_apk)))
  {
    s_apk[0] = '\0';
    logPrintf("setup: no .apk in the game folder\n");
    __atomic_store_n(&s_state, ST_NO_APK, __ATOMIC_RELEASE);
  }
  else if (apk_scan())
  {
    __atomic_store_n(&s_state, ST_READY, __ATOMIC_RELEASE);
  }

  if (!ui_init())
  {
    logPrintf("setup: no display\n");
    return 0;
  }
  pad = setup_open_pad();

  while (result < 0)
  {
    int a = 0, b = 0;
    int state = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    Uint32 now;

    // PortMaster's mapping on these devices is by label: SDL A = the button printed A
    if (pad)
    {
      SDL_GameControllerUpdate();
      a = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
      b = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
    }

    if (a && !prev_a)
    {
      logPrintf("setup: A pressed\n");
      if (state == ST_READY)
      {
        FILE *marker = fopen(INSTALL_MARKER, "w");
        if (marker)
          fclose(marker);
        s_done = 0;
        __atomic_store_n(&s_state, ST_INSTALLING, __ATOMIC_RELEASE);
        if (pthread_create(&thread, NULL, install_thread, NULL) == 0)
          thread_started = 1;
        else
          set_failed("%s", "Cannot start the install thread");
      }
      else if (state == ST_DONE)
      {
        result = 1;
      }
    }
    if (b && !prev_b && state != ST_INSTALLING)
    {
      logPrintf("setup: B pressed\n");
      result = 0;
    }
    prev_a = a;
    prev_b = b;

    // redraw on a state change or every UI_REDRAW_MS; leaves the CPU to the unzip
    now = SDL_GetTicks();
    state = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
    if (state != last_state || now - last_draw >= UI_REDRAW_MS)
    {
      ui_frame();
      last_draw = now;
      last_state = state;
    }
    else
    {
      SDL_Delay(UI_POLL_MS);
    }
  }

  if (thread_started)
    pthread_join(thread, NULL);
  if (pad)
    SDL_GameControllerClose(pad);
  ui_shutdown();

  logPrintf("setup: %s\n", result ? "launching" : "exit");
  return result;
}

// ---------------------------------------------------------------------------
// loading screen: background (centered crop), banner centered at 60% width,
// "LOADING..." under it. Drawn once here, then kept on every frame the game
// presents without drawing anything, until its own frames start.
// ---------------------------------------------------------------------------

static unsigned int s_sp_bg, s_sp_banner, s_sp_text;
static int s_sp_bg_w, s_sp_bg_h;
static float s_sp_bx, s_sp_by, s_sp_bw, s_sp_bh; // banner rectangle
static float s_sp_tx, s_sp_ty, s_sp_tw, s_sp_th; // text rectangle

static void splash_draw(void)
{
  if (s_sp_bg)
    draw_fill(s_sp_bg, s_sp_bg_w, s_sp_bg_h);
  if (s_sp_banner)
    video_draw_tex(s_sp_banner, s_sp_bx, s_sp_by, s_sp_bw, s_sp_bh, 0.0f, 0.0f, 1.0f, 1.0f, 1);
  if (s_sp_text)
    video_draw_tex(s_sp_text, s_sp_tx, s_sp_ty, s_sp_tw, s_sp_th, 0.0f, 0.0f, 1.0f, 1.0f, 1);
}

static void splash_release(void)
{
  if (s_sp_bg)
    video_tex_free(s_sp_bg);
  if (s_sp_banner)
    video_tex_free(s_sp_banner);
  if (s_sp_text)
    video_tex_free(s_sp_text);
  s_sp_bg = s_sp_banner = s_sp_text = 0;
}

// text into its own small texture: the overlay helpers draw into s_ov sized
// s_pw x s_ph, so they are pointed at a text-sized buffer for this
static unsigned int splash_text_texture(const char *text, int px, int *tw, int *th)
{
  int panel_w = s_pw, panel_h = s_ph;
  unsigned int tex = 0;
  int w, h;

  font_load();
  w = text_width(text, px) + 4;
  h = px * 4 / 3;
  s_ov = calloc((size_t)w * h, 4);
  if (s_ov)
  {
    s_pw = w;
    s_ph = h;
    text_draw(2, 0, px, text, COL_GOLD);
    tex = video_tex_create(s_ov, w, h);
    free(s_ov);
    s_ov = NULL;
  }
  s_pw = panel_w;
  s_ph = panel_h;

  free(s_font_data);
  s_font_data = NULL;
  s_have_font = 0;

  *tw = w;
  *th = h;
  return tex;
}

void splash_show(void)
{
  unsigned char *bg, *banner;
  int bw = 0, bh = 0, gw = 0, gh = 0, tw = 0, th = 0;
  int px, gap, have_bg, have_banner;
  float group_h;

  video_panel_size(&s_pw, &s_ph);
  if (s_pw <= 0 || s_ph <= 0)
    return;

  if (!s_apk[0])
    find_apk(s_apk, sizeof(s_apk)); // fallback source for the images
  banner_ensure();

  bg = image_load(APK_BG, APK_BG, &bw, &bh);
  banner = image_load(BANNER_NAME, APK_BANNER, &gw, &gh);
  have_bg = bg != NULL;
  have_banner = banner != NULL;

  if (bg)
  {
    s_sp_bg = video_tex_create(bg, bw, bh);
    s_sp_bg_w = bw;
    s_sp_bg_h = bh;
  }

  px = s_ph / 11; // the installer's body text size
  gap = px / 2;
  s_sp_text = splash_text_texture("LOADING...", px, &tw, &th);

  s_sp_bw = s_sp_bh = 0.0f;
  if (banner)
  {
    s_sp_banner = video_tex_create(banner, gw, gh);
    s_sp_bw = (float)s_pw * 0.6f;
    s_sp_bh = s_sp_bw * (float)gh / (float)gw;
  }

  // banner and text centered on screen as one group
  group_h = s_sp_bh + (s_sp_banner ? (float)gap : 0.0f) + (float)th;
  s_sp_bx = ((float)s_pw - s_sp_bw) * 0.5f;
  s_sp_by = ((float)s_ph - group_h) * 0.5f;
  s_sp_tw = (float)tw;
  s_sp_th = (float)th;
  s_sp_tx = ((float)s_pw - s_sp_tw) * 0.5f;
  s_sp_ty = s_sp_by + s_sp_bh + (s_sp_banner ? (float)gap : 0.0f);

  // presented more than once: a single present doesn't reach the panel
  // (the installer, which presents every 100 ms, always displays)
  for (int i = 0; i < 3; i++)
  {
    video_clear();
    splash_draw();
    video_present();
  }

  stbi_image_free(bg);
  stbi_image_free(banner);

  video_set_loading_screen(splash_draw, splash_release);
  logPrintf("splash: background %s, banner %s\n", have_bg ? "yes" : "no", have_banner ? "yes" : "no");
}