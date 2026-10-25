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

// before the game runs: starts the encoder threads
void texconv_init(void);
// one log line: textures converted so far, their size and the time spent
void texconv_log(void);

// import wrappers (imports.c dynlib table)
const GLubyte *texconv_glGetString(GLenum name);
void texconv_glGetIntegerv(GLenum pname, GLint *data);
void texconv_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
void texconv_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                    GLsizei height, GLint border, GLsizei size, const void *data);
void texconv_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                       GLsizei height, GLenum format, GLsizei size, const void *data);

#ifdef __cplusplus
}
#endif

#endif