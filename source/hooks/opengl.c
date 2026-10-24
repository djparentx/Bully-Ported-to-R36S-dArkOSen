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
  const char *s = getenv("BULLY_RENDER_SCALE");
  float scale = s ? (float)atof(s) : 1.0f;
  if (scale < 0.5f)
    scale = 0.5f;
  if (scale > 1.0f)
    scale = 1.0f;
  return scale;
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

int NVEventEGLInit(void)
{
  SDL_DisplayMode mode;

  if (s_window)
    return 1;

  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
  {
    debugPrintf("video: SDL video init failed: %s\n", SDL_GetError());
    return 0;
  }

  if (SDL_GetDesktopDisplayMode(0, &mode) != 0)
  {
    debugPrintf("video: no display mode: %s\n", SDL_GetError());
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
    debugPrintf("video: window creation failed: %s\n", SDL_GetError());
    return 0;
  }

  // creates the context and makes it current on this (main) thread
  s_glctx = SDL_GL_CreateContext(s_window);
  if (!s_glctx)
  {
    debugPrintf("video: GL context creation failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(s_window);
    s_window = NULL;
    return 0;
  }

  SDL_ShowCursor(SDL_DISABLE);

  g_egl_display = eglGetCurrentDisplay();
  g_egl_surface = eglGetCurrentSurface(EGL_DRAW);
  g_egl_context = eglGetCurrentContext();

  if (SDL_GL_SetSwapInterval(1) != 0)
    debugPrintf("video: could not set swap interval: %s\n", SDL_GetError());

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

  debugPrintf("video: panel %dx%d, game renders %dx%d (scale %.2f, fbo %u)\n",
              s_panel_w, s_panel_h, screen_width, screen_height, scale, s_fbo);
  debugPrintf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
  debugPrintf("GL_VERSION: %s\n", glGetString(GL_VERSION));
  debugPrintf("GL_EXTENSIONS: %s\n", glGetString(GL_EXTENSIONS));

  return 1; // success
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