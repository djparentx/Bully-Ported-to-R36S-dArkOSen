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
#include "stb_easy_font.h"

#include "config.h"
#include "hooks.h"
#include "util.h"
#include "setup.h"

#define APK_LIB_GAME   "lib/arm64-v8a/libGame.so"
#define APK_LIB_CPP    "lib/arm64-v8a/libc++_shared.so"
#define CPP_LIB_NAME   "libc++_shared.so"
#define INSTALL_MARKER ".installing"
#define SPLASH_IMAGE   "assets/flutter_assets/assets/images/bg.horizontal.png"
#define UI_W           320
#define EXTRACT_BUF    (1 << 20)
#define SPACE_MARGIN   (64ULL << 20)

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

static uint8_t *s_ui;
static int s_ui_w, s_ui_h;

static const uint8_t COL_BG[4] = {0x14, 0x14, 0x1c, 0xff};
static const uint8_t COL_TEXT[4] = {0xe8, 0xe8, 0xe8, 0xff};
static const uint8_t COL_DIM[4] = {0x90, 0x90, 0x98, 0xff};
static const uint8_t COL_ACCENT[4] = {0xf0, 0xb0, 0x30, 0xff};
static const uint8_t COL_ERR[4] = {0xf0, 0x50, 0x40, 0xff};

// ---------------------------------------------------------------------------
// install state
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
  debugPrintf("setup: FAILED: %s\n", s_msg);
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
    else
      assets++;
  }
  unzClose(zf);

  debugPrintf("setup: %s: %d asset files, %llu MB, libGame.so %s, libc++_shared.so %s\n",
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
    snprintf(s_current, sizeof(s_current), "%s", name);
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
        debugPrintf("setup: %3d%%  %s\n", tenth * 10, name);
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
  debugPrintf("setup: install complete\n");
  __atomic_store_n(&s_state, ST_DONE, __ATOMIC_RELEASE);

out:
  if (zf)
    unzClose(zf);
  free(buf);
  return NULL;
}

// ---------------------------------------------------------------------------
// UI: a small RGBA buffer with the panel's aspect, scaled up by video_draw_rgba
// ---------------------------------------------------------------------------

static int ui_alloc(void)
{
  int pw = 0, ph = 0;

  video_panel_size(&pw, &ph);
  if (pw <= 0 || ph <= 0)
    return 0;
  s_ui_w = UI_W;
  s_ui_h = UI_W * ph / pw;
  s_ui = malloc((size_t)s_ui_w * s_ui_h * 4);
  return s_ui != NULL;
}

static void ui_free(void)
{
  free(s_ui);
  s_ui = NULL;
}

static void ui_fill(int x, int y, int w, int h, const uint8_t col[4])
{
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w > s_ui_w ? s_ui_w : x + w;
  int y1 = y + h > s_ui_h ? s_ui_h : y + h;

  for (int yy = y0; yy < y1; yy++)
  {
    uint8_t *p = s_ui + ((size_t)yy * s_ui_w + x0) * 4;
    for (int xx = x0; xx < x1; xx++, p += 4)
      memcpy(p, col, 4);
  }
}

// stb_easy_font emits axis-aligned quads (16-byte vertices: x, y, z, rgba); fill each one
static void ui_text(int x, int y, const uint8_t col[4], const char *fmt, ...)
{
  static char vbuf[64 * 4096];
  char text[256];
  va_list ap;
  int quads;

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);

  quads = stb_easy_font_print((float)x, (float)y, text, NULL, vbuf, sizeof(vbuf));
  for (int q = 0; q < quads; q++)
  {
    const float *v0 = (const float *)(vbuf + q * 64);
    const float *v2 = (const float *)(vbuf + q * 64 + 32);
    int x0 = (int)v0[0], y0 = (int)v0[1];
    ui_fill(x0, y0, (int)v2[0] - x0, (int)v2[1] - y0, col);
  }
}

static void ui_draw(void)
{
  int y = 16;

  ui_fill(0, 0, s_ui_w, s_ui_h, COL_BG);
  ui_text(12, y, COL_ACCENT, "BULLY: ANNIVERSARY EDITION - SETUP");
  y += 24;

  switch (__atomic_load_n(&s_state, __ATOMIC_ACQUIRE))
  {
  case ST_NO_APK:
    ui_text(12, y, COL_TEXT, "Game data not installed.");
    y += 14;
    ui_text(12, y, COL_TEXT, "Copy your Bully APK (v1.4.311, arm64)");
    y += 12;
    ui_text(12, y, COL_TEXT, "into ports/bully, then start again.");
    y += 24;
    ui_text(12, y, COL_DIM, "SELECT: exit");
    break;

  case ST_READY:
    ui_text(12, y, COL_TEXT, "APK: %.40s", s_apk);
    y += 14;
    ui_text(12, y, COL_TEXT, "Needs %llu MB, %llu MB free", s_total >> 20, s_free >> 20);
    y += 24;
    ui_text(12, y, COL_DIM, "START: install    SELECT: exit");
    break;

  case ST_INSTALLING:
  {
    unsigned long long done, total;
    char current[96];
    int pct;

    pthread_mutex_lock(&s_lock);
    done = s_done;
    total = s_total;
    snprintf(current, sizeof(current), "%s", s_current);
    pthread_mutex_unlock(&s_lock);
    pct = total ? (int)(done * 100 / total) : 0;

    ui_text(12, y, COL_TEXT, "Installing... please wait");
    y += 18;
    ui_fill(12, y, s_ui_w - 24, 10, COL_DIM);
    ui_fill(13, y + 1, (s_ui_w - 26) * pct / 100, 8, COL_ACCENT);
    y += 18;
    ui_text(12, y, COL_TEXT, "%3d%%  %.40s", pct, current);
    break;
  }

  case ST_DONE:
    ui_text(12, y, COL_TEXT, "Installed.");
    y += 24;
    ui_text(12, y, COL_DIM, "START: launch    SELECT: exit");
    break;

  default:
    ui_text(12, y, COL_ERR, "Setup failed:");
    y += 14;
    ui_text(12, y, COL_TEXT, "%.50s", s_msg);
    y += 24;
    ui_text(12, y, COL_DIM, "SELECT: exit");
    break;
  }

  video_draw_rgba(s_ui, s_ui_w, s_ui_h, 1);
  video_present();
}

static SDL_GameController *setup_open_pad(void)
{
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
  {
    debugPrintf("setup: controller init failed: %s\n", SDL_GetError());
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
      debugPrintf("setup: controller %s\n", SDL_GameControllerName(pad));
      return pad;
    }
  }
  debugPrintf("setup: no game controller found\n");
  return NULL;
}

int setup_run(void)
{
  SDL_GameController *pad;
  pthread_t thread;
  int thread_started = 0;
  int prev_start = 1, prev_back = 1; // a button must be released before it counts
  int result = -1;

  if (!ui_alloc())
  {
    debugPrintf("setup: no display\n");
    return 0;
  }
  pad = setup_open_pad();

  if (!find_apk(s_apk, sizeof(s_apk)))
  {
    debugPrintf("setup: no .apk in the game folder\n");
    __atomic_store_n(&s_state, ST_NO_APK, __ATOMIC_RELEASE);
  }
  else if (apk_scan())
  {
    __atomic_store_n(&s_state, ST_READY, __ATOMIC_RELEASE);
  }

  while (result < 0)
  {
    int start = 0, back = 0;
    int state = __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);

    if (pad)
    {
      SDL_GameControllerUpdate();
      start = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
      back = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK);
    }

    if (start && !prev_start)
    {
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
    if (back && !prev_back && state != ST_INSTALLING)
      result = 0;

    prev_start = start;
    prev_back = back;

    ui_draw(); // the swap (vsync) paces this loop
  }

  if (thread_started)
    pthread_join(thread, NULL);
  if (pad)
    SDL_GameControllerClose(pad);
  ui_free();

  debugPrintf("setup: %s\n", result ? "launching" : "exit");
  return result;
}

// ---------------------------------------------------------------------------
// loading splash (from the user's own install)
// ---------------------------------------------------------------------------

void splash_show(void)
{
  int w, h, n;
  unsigned char *img = stbi_load(SPLASH_IMAGE, &w, &h, &n, 4);

  if (img)
  {
    debugPrintf("splash: %dx%d image\n", w, h);
    video_draw_rgba(img, w, h, 0);
    video_present();
    stbi_image_free(img);
    return;
  }

  debugPrintf("splash: %s: %s, text fallback\n", SPLASH_IMAGE, stbi_failure_reason());
  if (!ui_alloc())
    return;
  ui_fill(0, 0, s_ui_w, s_ui_h, COL_BG);
  ui_text(12, s_ui_h / 2 - 4, COL_TEXT, "Loading...");
  video_draw_rgba(s_ui, s_ui_w, s_ui_h, 1);
  video_present();
  ui_free();
}
