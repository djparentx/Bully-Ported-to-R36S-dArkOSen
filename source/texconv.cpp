/* texconv.cpp -- DXT (S3TC) game textures converted to ETC2 for Mali
 *
 * Mali has no S3TC. Without it the game decodes every DXT texture on the CPU
 * (Texture2DES::CreateCompatibleImage) into 16-bit textures: DXT1 -> RGB565
 * (2 bytes/pixel instead of 0.5), DXT3/DXT5 -> RGBA4444 (2 bytes/pixel
 * instead of 1, and 4-bit colour).
 *
 * Here S3TC is reported as supported (GL_EXTENSIONS and the compressed format
 * list), so the game uploads its DXT data as it would on a GPU with S3TC:
 *   glTexStorage2D(DXT format)               -> ETC2 storage
 *   glCompressedTexSubImage2D(DXT data) * N  -> decoded (bcdec), re-encoded
 *   glCompressedTexImage2D(DXT data)            to ETC2 (etcpak), uploaded
 * DXT1 -> ETC2 RGB (0.5 bytes/pixel), DXT3/DXT5 -> ETC2 RGBA8 (1 byte/pixel).
 * DXT1 is treated as opaque, as the game does with S3TC (GL_COMPRESSED_RGB_S3TC_DXT1).
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

#define DXT1 0x83F0 // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define DXT1A 0x83F1 // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define DXT3 0x83F2 // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define DXT5 0x83F3 // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define S3TC_EXTENSION "GL_EXT_texture_compression_s3tc"

#define WORKERS 3              // plus the calling thread
#define SPLIT_MIN_ROWS 16      // block rows below which one thread encodes
#define BUFFER_KEEP (4u << 20) // scratch kept between textures (a 1024x1024 level)

static uint32_t *s_pixels = NULL; // decoded block rows (BGRA for etcpak)
static size_t s_pixels_cap = 0;
static uint64_t *s_blocks = NULL; // ETC2 output
static size_t s_blocks_cap = 0;

static unsigned s_textures = 0;
static unsigned s_failed = 0;
static uint64_t s_bytes = 0;
static uint64_t s_ns = 0;

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

  // bcdec writes R,G,B,A bytes; etcpak reads B,G,R,A. DXT1 is opaque
  // (GL_COMPRESSED_RGB_S3TC_DXT1 semantics).
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

static GLenum etc2_format(int kind)
{
  return kind == 1 ? GL_COMPRESSED_RGB8_ETC2 : GL_COMPRESSED_RGBA8_ETC2_EAC;
}

static void upload_level(int full, GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLsizei width, GLsizei height, int kind, GLsizei size, const void *data)
{
  const uint64_t t0 = mono_ns();
  const int rgba = kind != 1;
  const size_t out = transcode(kind, rgba, width, height, size, data);

  if (out)
  {
    if (full)
      glCompressedTexImage2D(target, level, etc2_format(kind), width, height, 0, (GLsizei)out, s_blocks);
    else
      glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, etc2_format(kind),
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

// --- import wrappers -------------------------------------------------------------

// S3TC is reported as supported, so the game skips its own CPU decode and
// uploads the DXT data, which the wrappers below turn into ETC2
extern "C" const GLubyte *texconv_glGetString(GLenum name)
{
  static char *s_extensions = NULL;
  const char *real = (const char *)glGetString(name);

  if (name != GL_EXTENSIONS || !real)
    return (const GLubyte *)real;
  if (!s_extensions)
  {
    const size_t len = strlen(real);
    char *ext = (char *)malloc(len + sizeof(" " S3TC_EXTENSION));

    if (!ext)
      return (const GLubyte *)real;
    memcpy(ext, real, len);
    memcpy(ext + len, " " S3TC_EXTENSION, sizeof(" " S3TC_EXTENSION));
    s_extensions = ext;
  }
  return (const GLubyte *)s_extensions;
}

extern "C" void texconv_glGetIntegerv(GLenum pname, GLint *data)
{
  if (pname == GL_COMPRESSED_TEXTURE_FORMATS && data)
  {
    GLint n = 0;

    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    glGetIntegerv(pname, data);
    data[n + 0] = DXT1;
    data[n + 1] = DXT1A;
    data[n + 2] = DXT3;
    data[n + 3] = DXT5;
    return;
  }
  glGetIntegerv(pname, data);
  if (pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS && data)
    data[0] += 4;
}

extern "C" void texconv_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                       GLsizei width, GLsizei height)
{
  const int kind = dxt_kind(internalformat);

  if (kind)
  {
    internalformat = etc2_format(kind);
    s_textures++;
  }
  glTexStorage2D(target, levels, internalformat, width, height);
}

extern "C" void texconv_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                               GLsizei width, GLsizei height, GLint border,
                                               GLsizei size, const void *data)
{
  const int kind = dxt_kind(internalformat);

  if (!kind || !data)
  {
    glCompressedTexImage2D(target, level, internalformat, width, height, border, size, data);
    return;
  }
  if (level == 0)
    s_textures++;
  upload_level(1, target, level, 0, 0, width, height, kind, size, data);
}

extern "C" void texconv_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                  GLsizei width, GLsizei height, GLenum format,
                                                  GLsizei size, const void *data)
{
  const int kind = dxt_kind(format);

  if (!kind || !data)
  {
    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, size, data);
    return;
  }
  upload_level(0, target, level, xoffset, yoffset, width, height, kind, size, data);
}

extern "C" void texconv_init(void)
{
  pool_start();
  logPrintf("textures: DXT to ETC2 conversion on (%d threads)\n", s_pool_ready ? WORKERS + 1 : 1);
}

extern "C" void texconv_log(void)
{
  logPrintf("textures: %u converted to ETC2, %.1f MB uploaded, %.1f s converting, %u failed\n",
            s_textures, (double)s_bytes / (1024.0 * 1024.0), (double)s_ns / 1e9, s_failed);
}