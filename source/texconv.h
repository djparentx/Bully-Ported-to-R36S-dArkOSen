/* texconv.h -- DXT (S3TC) game textures converted to ETC2 for Mali
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __TEXCONV_H__
#define __TEXCONV_H__

#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif

// after so_finalize(): takes over the game's DXT upload (GOT patch)
void texconv_patch(void *text_virtbase);
// one log line: textures converted so far, their size and the time spent
void texconv_log(void);

// import wrappers (imports.c dynlib table)
void texconv_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
void texconv_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                             GLsizei height, GLenum format, GLenum type, const void *pixels);
void texconv_glDeleteTextures(GLsizei n, const GLuint *textures);

#ifdef __cplusplus
}
#endif

#endif
