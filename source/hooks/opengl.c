/* opengl.c -- video: SDL2 KMSDRM window + GLES context, swap and optional render scaling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include "../config.h"
#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"

// Global EGL objects - read back from SDL's context; used by the EGL wrappers in imports.c
EGLDisplay g_egl_display = EGL_NO_DISPLAY;
EGLSurface g_egl_surface = EGL_NO_SURFACE;
EGLContext g_egl_context = EGL_NO_CONTEXT;

static SDL_Window *s_window = NULL;
static SDL_GLContext s_glctx = NULL;
static int s_panel_w = 0, s_panel_h = 0;

// Render scale < 1.0: the game draws into this FBO at screen_width x screen_height
// (framebuffer 0 is redirected to it) and video_swap() scales it to the panel.
static GLuint s_fbo = 0, s_fbo_color = 0, s_fbo_depth = 0;

static float video_render_scale(void)
{
  return config.render_scale; // clamped to 0.50-1.00 by read_config()
}

static int video_create_fbo(void)
{
  glGenRenderbuffers(1, &s_fbo_color);
  glBindRenderbuffer(GL_RENDERBUFFER, s_fbo_color);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, screen_width, screen_height);

  glGenRenderbuffers(1, &s_fbo_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, s_fbo_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, screen_width, screen_height);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glGenFramebuffers(1, &s_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_fbo_color);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_fbo_depth);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    debugPrintf("video: FBO incomplete (0x%x), rendering at panel size\n", status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &s_fbo);
    glDeleteRenderbuffers(1, &s_fbo_color);
    glDeleteRenderbuffers(1, &s_fbo_depth);
    s_fbo = s_fbo_color = s_fbo_depth = 0;
    return 0;
  }

  // stays bound: the game's "framebuffer 0" is this FBO from now on
  return 1;
}

void NVEventEGLSwapBuffers(void)
{
  video_swap();
}

void NVEventEGLMakeCurrent(void)
{
}

void NVEventEGLUnmakeCurrent(void)
{
}

// Window + GLES context, current on the calling (main) thread.
// Opened once at startup and shared by the installer, the splash and the game.
int video_open(void)
{
  SDL_DisplayMode mode;

  if (s_window)
    return 1;

  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
  {
    logPrintf("video: SDL video init failed: %s\n", SDL_GetError());
    return 0;
  }

  if (SDL_GetDesktopDisplayMode(0, &mode) != 0)
  {
    logPrintf("video: no display mode: %s\n", SDL_GetError());
    return 0;
  }
  s_panel_w = mode.w;
  s_panel_h = mode.h;

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  s_window = SDL_CreateWindow("Bully", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              s_panel_w, s_panel_h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!s_window)
  {
    logPrintf("video: window creation failed: %s\n", SDL_GetError());
    return 0;
  }

  // creates the context and makes it current on this (main) thread
  s_glctx = SDL_GL_CreateContext(s_window);
  if (!s_glctx)
  {
    logPrintf("video: GL context creation failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(s_window);
    s_window = NULL;
    return 0;
  }

  SDL_ShowCursor(SDL_DISABLE);

  g_egl_display = eglGetCurrentDisplay();
  g_egl_surface = eglGetCurrentSurface(EGL_DRAW);
  g_egl_context = eglGetCurrentContext();

  if (SDL_GL_SetSwapInterval(1) != 0)
    logPrintf("video: could not set swap interval: %s\n", SDL_GetError());

  logPrintf("video: panel %dx%d\n", s_panel_w, s_panel_h);
  logPrintf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
  logPrintf("GL_VERSION: %s\n", glGetString(GL_VERSION));
  return 1;
}

// Game path (jni_start / InitEGLAndGLES2): reuse the window, set the render size, optional FBO
int NVEventEGLInit(void)
{
  static int s_game_ready = 0;

  if (s_game_ready)
    return 1;
  if (!video_open())
    return 0;

  float scale = video_render_scale();
  screen_width = s_panel_w;
  screen_height = s_panel_h;
  if (scale < 1.0f)
  {
    screen_width = ((int)(s_panel_w * scale)) & ~1;
    screen_height = ((int)(s_panel_h * scale)) & ~1;
    if (!video_create_fbo())
    {
      screen_width = s_panel_w;
      screen_height = s_panel_h;
    }
  }

  logPrintf("video: game renders %dx%d (scale %.2f, fbo %u)\n",
              screen_width, screen_height, scale, s_fbo);
  debugPrintf("GL_EXTENSIONS: %s\n", glGetString(GL_EXTENSIONS));

  s_game_ready = 1;
  return 1; // success
}

// --- textured quads, for the installer UI and the loading screen ----------

static GLuint s_blit_prog = 0;

static GLuint blit_compile(GLenum type, const char *src)
{
  GLuint sh = glCreateShader(type);
  GLint ok = 0;

  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[256];
    glGetShaderInfoLog(sh, sizeof(log), NULL, log);
    logPrintf("video: blit shader: %s\n", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static int blit_init(void)
{
  static const char *vs =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
  static const char *fs =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_tex;\n"
      "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";
  GLuint v = blit_compile(GL_VERTEX_SHADER, vs);
  GLuint f = blit_compile(GL_FRAGMENT_SHADER, fs);
  GLint ok = 0;

  if (!v || !f)
  {
    if (v)
      glDeleteShader(v);
    if (f)
      glDeleteShader(f);
    return 0;
  }

  s_blit_prog = glCreateProgram();
  glAttachShader(s_blit_prog, v);
  glAttachShader(s_blit_prog, f);
  glBindAttribLocation(s_blit_prog, 0, "a_pos");
  glBindAttribLocation(s_blit_prog, 1, "a_uv");
  glLinkProgram(s_blit_prog);
  glDeleteShader(v);
  glDeleteShader(f);
  glGetProgramiv(s_blit_prog, GL_LINK_STATUS, &ok);
  if (!ok)
  {
    logPrintf("video: blit program link failed\n");
    glDeleteProgram(s_blit_prog);
    s_blit_prog = 0;
    return 0;
  }

  glUseProgram(s_blit_prog);
  glUniform1i(glGetUniformLocation(s_blit_prog, "u_tex"), 0);
  glUseProgram(0);
  return 1;
}

unsigned int video_tex_create(const void *rgba, int w, int h)
{
  GLuint tex;

  glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  return tex;
}

void video_tex_update(unsigned int tex, const void *rgba, int w, int h)
{
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void video_tex_free(unsigned int tex)
{
  GLuint t = tex;
  glDeleteTextures(1, &t);
}

void video_clear(void)
{
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, s_panel_w, s_panel_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

// Draw a texture into a panel-pixel rectangle (top-left origin) with the
// given texture coordinates; blend = straight-alpha blending over what's there
void video_draw_tex(unsigned int tex, float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1, int blend)
{
  float x0, x1, y0, y1;

  if (!s_window || !tex)
    return;
  if (!s_blit_prog && !blit_init())
    return;

  x0 = x / (float)s_panel_w * 2.0f - 1.0f;
  x1 = (x + w) / (float)s_panel_w * 2.0f - 1.0f;
  y0 = 1.0f - y / (float)s_panel_h * 2.0f;
  y1 = 1.0f - (y + h) / (float)s_panel_h * 2.0f;

  const GLfloat verts[] = {
      x0, y1, u0, v1,
      x1, y1, u1, v1,
      x0, y0, u0, v0,
      x1, y0, u1, v0,
  };

  if (blend)
  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  else
  {
    glDisable(GL_BLEND);
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glUseProgram(s_blit_prog);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glUseProgram(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_BLEND);
}

void video_present(void)
{
  SDL_GL_SwapWindow(s_window);
}

void video_panel_size(int *w, int *h)
{
  *w = s_panel_w;
  *h = s_panel_h;
}

// Called on the thread that owns the context (render thread)
void video_swap(void)
{
  GLint draw_fb = 0, read_fb = 0;

  if (s_fbo)
  {
    static const GLenum fbo_discard[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    static const GLenum win_discard[] = {GL_COLOR, GL_DEPTH, GL_STENCIL};
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    if (scissor)
      glDisable(GL_SCISSOR_TEST);

    // depth/stencil are not needed past this point; the blit overwrites the whole window
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 2, fbo_discard);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 3, win_discard);
    glBlitFramebuffer(0, 0, screen_width, screen_height,
                      0, 0, s_panel_w, s_panel_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    if (scissor)
      glEnable(GL_SCISSOR_TEST);
  }

  SDL_GL_SwapWindow(s_window);

  if (s_fbo)
  {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
  }
}

// SDL tracks the current window per thread; SDL_GL_SwapWindow fails unless the
// context was made current through SDL, so every make/release goes through here.
int video_make_current(void)
{
  if (SDL_GL_MakeCurrent(s_window, s_glctx) != 0)
  {
    debugPrintf("video_make_current: failed: %s\n", SDL_GetError());
    return 0;
  }
  return 1;
}

void video_release_current(void)
{
  SDL_GL_MakeCurrent(s_window, NULL);
}

// Keep SDL's KMSDRM page-flip logic in sync with the game's swap interval
EGLBoolean eglSwapInterval_wrapper(EGLDisplay dpy, EGLint interval)
{
  (void)dpy;
  debugPrintf("eglSwapInterval_wrapper: %d\n", interval);
  return SDL_GL_SetSwapInterval(interval) == 0 ? EGL_TRUE : EGL_FALSE;
}

// The game's framebuffer 0 is our scaling FBO when render scale < 1.0
void glBindFramebuffer_wrapper(GLenum target, GLuint framebuffer)
{
  glBindFramebuffer(target, framebuffer == 0 ? s_fbo : framebuffer);
}

void patch_opengl(void)
{
  // --- NVEventEGL* symbols DO NOT EXIST in 64-bit libGame.so ---
  // hook_arm64(so_find_addr("_Z14NVEventEGLInitv"), (uintptr_t)NVEventEGLInit);
  // hook_arm64(so_find_addr("_Z21NVEventEGLMakeCurrentv"), (uintptr_t)NVEventEGLMakeCurrent);
  // hook_arm64(so_find_addr("_Z23NVEventEGLUnmakeCurrentv"), (uintptr_t)NVEventEGLUnmakeCurrent);
  // hook_arm64(so_find_addr("_Z21NVEventEGLSwapBuffersv"), (uintptr_t)NVEventEGLSwapBuffers);
}

void deinit_opengl(void)
{
  if (!s_window)
    return;

  // GL objects (incl. the FBO) go with the context
  if (s_glctx)
  {
    SDL_GL_MakeCurrent(s_window, NULL);
    SDL_GL_DeleteContext(s_glctx);
    s_glctx = NULL;
  }
  SDL_DestroyWindow(s_window);
  s_window = NULL;
  s_fbo = s_fbo_color = s_fbo_depth = 0;

  g_egl_display = EGL_NO_DISPLAY;
  g_egl_surface = EGL_NO_SURFACE;
  g_egl_context = EGL_NO_CONTEXT;

  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}