/* texconv.cpp -- DXT (S3TC) game textures converted to ETC2 for Mali
 *
 * Mali has no S3TC, so the game decodes every DXT texture on the CPU into
 * 16-bit storage: DXT1 -> RGB565 (2 bytes/pixel instead of 0.5) and
 * DXT3/DXT5 -> RGBA4444 (2 bytes/pixel instead of 1, and 4-bit colour).
 * Here each DXT texture is decoded (bcdec) and re-encoded to ETC2 (etcpak),
 * which Mali supports natively: DXT1 -> ETC2 RGB (0.5 bytes/pixel),
 * DXT3/DXT5 -> ETC2 RGBA8 (1 byte/pixel).
 *
 * The game's ES3 path (Texture2DES3::InitWithData) does:
 *   glTexStorage2D(RGB565 or RGBA4)          for a DXT texture
 *   wrapped_glCompressedTexImage2D(...) * N  one per mip level, sub-image
 * Native RGB565/RGBA4444 textures use the same storage formats but are filled
 * with glTexSubImage2D, so the storage call is held back until the first
 * upload shows which kind of texture it is.
 *
 * All calls come from the thread that holds the GL context, so the state here
 * needs no lock. Large textures are encoded on all four cores: the calling
 * thread and three workers each take a quarter of the block rows.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "texconv.h"
extern "C" {
#include "util.h"
}

#define BCDECDEF static inline // private to this file
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include "etcpak/ProcessRGB.hpp"

// libBully.so (APK v1.4.311): GOT slot and address of
// wrapped_glCompressedTexImage2D(unsigned, int, unsigned, int, int, int, int, const void *, bool, bool)
#define WRAPPED_COMPRESSED_GOT 0x1226f60
#define WRAPPED_COMPRESSED_FN 0x963dc8

#define DXT1 0x83F0 // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define DXT1A 0x83F1 // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define DXT3 0x83F2 // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define DXT5 0x83F3 // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT

#define MAX_TRACKED (1u << 20) // texture names followed (1 byte each)
#define WORKERS 3              // plus the calling thread
#define SPLIT_MIN_ROWS 16      // block rows below which one thread encodes
#define BUFFER_KEEP (4u << 20) // scratch kept between textures (a 1024x1024 level)

enum
{
  TRACK_NONE = 0,
  TRACK_ETC2_RGB = 1,
  TRACK_ETC2_RGBA = 2,
};

typedef void (*WrappedFn)(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height,
                          GLint border, GLsizei size, const void *data, bool sub, bool uncompressed);

static WrappedFn s_wrapped_real = NULL;
static uint8_t *s_tracked = NULL; // per texture name: TRACK_*

// a 16-bit storage call held back until the first upload
static struct
{
  int valid;
  GLuint tex;
  GLsizei levels, width, height;
  GLenum internalformat;
} s_pending;

static uint32_t *s_pixels = NULL; // decoded block rows (BGRA for etcpak)
static size_t s_pixels_cap = 0;
static uint64_t *s_blocks = NULL; // ETC2 output
static size_t s_blocks_cap = 0;

static unsigned s_textures = 0;
static unsigned s_failed = 0;
static uint64_t s_bytes = 0;
static uint64_t s_ns = 0;

static GLuint bound_texture(void)
{
  GLint tex = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  return (GLuint)tex;
}

// the held-back storage call goes through unchanged (a native 16-bit texture)
static void commit_pending(void)
{
  GLuint cur;

  if (!s_pending.valid)
    return;
  s_pending.valid = 0;
  cur = bound_texture();
  if (cur != s_pending.tex)
    glBindTexture(GL_TEXTURE_2D, s_pending.tex);
  glTexStorage2D(GL_TEXTURE_2D, s_pending.levels, s_pending.internalformat, s_pending.width, s_pending.height);
  if (cur != s_pending.tex)
    glBindTexture(GL_TEXTURE_2D, cur);
}

static int grow(void **buf, size_t *cap, size_t need)
{
  void *p;

  if (*cap >= need)
    return 1;
  p = realloc(*buf, need);
  if (!p)
    return 0;
  *buf = p;
  *cap = need;
  return 1;
}

static int dxt_kind(GLenum format)
{
  switch (format)
  {
  case DXT1:
  case DXT1A:
    return 1;
  case DXT3:
    return 3;
  case DXT5:
    return 5;
  default:
    return 0;
  }
}

// --- encoder workers -----------------------------------------------------------

typedef struct
{
  const uint32_t *src;
  uint64_t *dst;
  uint32_t blocks;
  size_t width;
  int rgba;
} EncodeJob;

static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_pool_go = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_pool_done = PTHREAD_COND_INITIALIZER;
static EncodeJob s_jobs[WORKERS];
static unsigned s_pool_gen = 0;
static unsigned s_pool_left = 0;
static int s_pool_ready = 0;

static void encode_job(const EncodeJob *j)
{
  if (j->blocks == 0)
    return;
  if (j->rgba)
    CompressEtc2Rgba(j->src, j->dst, j->blocks, j->width, false);
  else
    CompressEtc2Rgb(j->src, j->dst, j->blocks, j->width, false);
}

static void *pool_worker(void *arg)
{
  const int id = (int)(intptr_t)arg;
  unsigned seen = 0;

  for (;;)
  {
    EncodeJob job;

    pthread_mutex_lock(&s_pool_lock);
    while (s_pool_gen == seen)
      pthread_cond_wait(&s_pool_go, &s_pool_lock);
    seen = s_pool_gen;
    job = s_jobs[id];
    pthread_mutex_unlock(&s_pool_lock);

    encode_job(&job);

    pthread_mutex_lock(&s_pool_lock);
    if (--s_pool_left == 0)
      pthread_cond_signal(&s_pool_done);
    pthread_mutex_unlock(&s_pool_lock);
  }
  return NULL;
}

static void pool_start(void)
{
  pthread_attr_t attr;
  int started = 0;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (int i = 0; i < WORKERS; i++)
  {
    pthread_t t;
    if (pthread_create(&t, &attr, pool_worker, (void *)(intptr_t)i) == 0)
      started++;
  }
  pthread_attr_destroy(&attr);
  s_pool_ready = started == WORKERS; // all or nothing: jobs are handed out per worker
}

// s_pixels (bw*4 x bh*4 BGRA) -> s_blocks
static void encode(int rgba, size_t bw, size_t bh)
{
  const size_t width = bw * 4;
  const size_t per_block = rgba ? 2 : 1; // uint64 words per ETC2 block
  EncodeJob last;

  if (!s_pool_ready || bh < SPLIT_MIN_ROWS)
  {
    EncodeJob all = {s_pixels, s_blocks, (uint32_t)(bw * bh), width, rgba};
    encode_job(&all);
    return;
  }

  {
    const size_t rows = bh / (WORKERS + 1);
    size_t row = 0;

    pthread_mutex_lock(&s_pool_lock);
    for (int i = 0; i < WORKERS; i++, row += rows)
    {
      s_jobs[i].src = s_pixels + row * 4 * width;
      s_jobs[i].dst = s_blocks + row * bw * per_block;
      s_jobs[i].blocks = (uint32_t)(rows * bw);
      s_jobs[i].width = width;
      s_jobs[i].rgba = rgba;
    }
    s_pool_left = WORKERS;
    s_pool_gen++;
    pthread_cond_broadcast(&s_pool_go);
    pthread_mutex_unlock(&s_pool_lock);

    // the rest (the last quarter plus any remainder) on this thread
    last.src = s_pixels + row * 4 * width;
    last.dst = s_blocks + row * bw * per_block;
    last.blocks = (uint32_t)((bh - row) * bw);
    last.width = width;
    last.rgba = rgba;
    encode_job(&last);

    pthread_mutex_lock(&s_pool_lock);
    while (s_pool_left != 0)
      pthread_cond_wait(&s_pool_done, &s_pool_lock);
    pthread_mutex_unlock(&s_pool_lock);
  }
}

// DXT level -> ETC2 blocks in s_blocks; returns the ETC2 size in bytes, 0 on bad input
static size_t transcode(int kind, int rgba, GLsizei width, GLsizei height, GLsizei size, const void *data)
{
  const size_t bw = ((size_t)width + 3) / 4, bh = ((size_t)height + 3) / 4;
  const size_t blocks = bw * bh;
  const size_t in_block = kind == 1 ? 8 : 16;
  const size_t pitch = bw * 4 * 4; // bytes per decoded pixel row
  const uint8_t *src = (const uint8_t *)data;
  size_t bx, by, i;

  if (width <= 0 || height <= 0 || (size_t)size < blocks * in_block)
    return 0;
  if (!grow((void **)&s_pixels, &s_pixels_cap, blocks * 16 * sizeof(uint32_t)) ||
      !grow((void **)&s_blocks, &s_blocks_cap, blocks * (rgba ? 2 : 1) * sizeof(uint64_t)))
    return 0;

  for (by = 0; by < bh; by++)
  {
    for (bx = 0; bx < bw; bx++)
    {
      uint8_t *dst = (uint8_t *)s_pixels + by * 4 * pitch + bx * 16;

      if (kind == 1)
        bcdec_bc1(src, dst, (int)pitch);
      else if (kind == 3)
        bcdec_bc2(src, dst, (int)pitch);
      else
        bcdec_bc3(src, dst, (int)pitch);
      src += in_block;
    }
  }

  // bcdec writes R,G,B,A bytes; etcpak reads B,G,R,A. DXT1 is opaque, as in
  // the game's own RGB565 path.
  for (i = 0; i < blocks * 16; i++)
  {
    uint32_t v = s_pixels[i];

    v = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
    if (kind == 1)
      v |= 0xFF000000u;
    s_pixels[i] = v;
  }

  encode(rgba, bw, bh);
  return blocks * (rgba ? 16 : 8);
}

static void wrapped_hook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height,
                         GLint border, GLsizei size, const void *data, bool sub, bool uncompressed)
{
  const int kind = dxt_kind(format);
  uint64_t t0;
  size_t out;
  GLuint tex;
  int rgba;

  if (!kind || target != GL_TEXTURE_2D || !data)
  {
    commit_pending();
    s_wrapped_real(target, level, format, width, height, border, size, data, sub, uncompressed);
    return;
  }

  tex = bound_texture();
  if (s_pending.valid && s_pending.tex == tex)
  {
    // the 16-bit storage this DXT texture asked for becomes ETC2 instead
    const int to_rgba = s_pending.internalformat == GL_RGBA4;

    s_pending.valid = 0;
    glTexStorage2D(GL_TEXTURE_2D, s_pending.levels,
                   to_rgba ? GL_COMPRESSED_RGBA8_ETC2_EAC : GL_COMPRESSED_RGB8_ETC2,
                   s_pending.width, s_pending.height);
    s_tracked[tex] = to_rgba ? TRACK_ETC2_RGBA : TRACK_ETC2_RGB;
    s_textures++;
  }
  else
  {
    commit_pending();
  }

  if (tex == 0 || tex >= MAX_TRACKED || s_tracked[tex] == TRACK_NONE)
  {
    s_wrapped_real(target, level, format, width, height, border, size, data, sub, uncompressed);
    return;
  }

  rgba = s_tracked[tex] == TRACK_ETC2_RGBA;
  t0 = mono_ns();
  out = transcode(kind, rgba, width, height, size, data);
  if (out)
  {
    glCompressedTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, width, height,
                              rgba ? GL_COMPRESSED_RGBA8_ETC2_EAC : GL_COMPRESSED_RGB8_ETC2,
                              (GLsizei)out, s_blocks);
    s_bytes += out;
  }
  else
  {
    s_failed++;
  }
  s_ns += mono_ns() - t0;

  // keep the scratch buffers for ordinary sizes, give big ones back
  if (s_pixels_cap > BUFFER_KEEP)
  {
    free(s_pixels);
    free(s_blocks);
    s_pixels = NULL;
    s_blocks = NULL;
    s_pixels_cap = s_blocks_cap = 0;
  }
}

extern "C" void texconv_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                       GLsizei width, GLsizei height)
{
  commit_pending();
  if (s_tracked && target == GL_TEXTURE_2D && levels > 0 &&
      (internalformat == GL_RGB565 || internalformat == GL_RGBA4))
  {
    GLuint tex = bound_texture();

    if (tex != 0 && tex < MAX_TRACKED)
    {
      s_tracked[tex] = TRACK_NONE;
      s_pending.valid = 1;
      s_pending.tex = tex;
      s_pending.levels = levels;
      s_pending.internalformat = internalformat;
      s_pending.width = width;
      s_pending.height = height;
      return;
    }
  }
  glTexStorage2D(target, levels, internalformat, width, height);
}

extern "C" void texconv_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                        GLsizei width, GLsizei height, GLenum format, GLenum type,
                                        const void *pixels)
{
  commit_pending();
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

extern "C" void texconv_glDeleteTextures(GLsizei n, const GLuint *textures)
{
  if (s_tracked && textures)
  {
    for (GLsizei i = 0; i < n; i++)
    {
      if (s_pending.valid && textures[i] == s_pending.tex)
        s_pending.valid = 0;
      if (textures[i] < MAX_TRACKED)
        s_tracked[textures[i]] = TRACK_NONE;
    }
  }
  glDeleteTextures(n, textures);
}

extern "C" void texconv_patch(void *text_virtbase)
{
  uintptr_t *slot = (uintptr_t *)((uintptr_t)text_virtbase + WRAPPED_COMPRESSED_GOT);

  if (*slot != (uintptr_t)text_virtbase + WRAPPED_COMPRESSED_FN)
  {
    logPrintf("textures: DXT to ETC2 off (unexpected libBully.so)\n");
    return;
  }
  s_tracked = (uint8_t *)calloc(MAX_TRACKED, 1);
  if (!s_tracked)
  {
    logPrintf("textures: DXT to ETC2 off (out of memory)\n");
    return;
  }
  pool_start();
  s_wrapped_real = (WrappedFn)*slot;
  *slot = (uintptr_t)wrapped_hook;
  logPrintf("textures: DXT to ETC2 conversion on (%d threads)\n", s_pool_ready ? WORKERS + 1 : 1);
}

extern "C" void texconv_log(void)
{
  if (!s_tracked)
    return;
  logPrintf("textures: %u converted to ETC2, %.1f MB uploaded, %.1f s converting, %u failed\n",
            s_textures, (double)s_bytes / (1024.0 * 1024.0), (double)s_ns / 1e9, s_failed);
}
