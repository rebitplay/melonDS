// melonDS's OpenGL renderer on WebGL2 (OpenGL ES 3.0 through Emscripten): the GL header the
// renderer includes (MELONDS_GL_HEADER), with the few desktop entry points it uses mapped onto
// ES 3.0 and WebGL2 extensions. Texture uploads and readbacks in the DS's own 16-bit format
// (GL_UNSIGNED_SHORT_1_5_5_5_REV, which ES lacks) go through rebitGL helpers at their call sites.
#ifndef REBIT_GL_H
#define REBIT_GL_H

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <cstddef>
#include <cstdint>

// per-draw-buffer colour mask: OES_draw_buffers_indexed; without it, buffer 0 only
void rebitColorMaski(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
#define glColorMaski rebitColorMaski
bool rebitHasDrawBuffersIndexed();

static inline void rebitDrawBuffer(GLenum buffer)
{
    glDrawBuffers(1, &buffer);
}
#define glDrawBuffer rebitDrawBuffer

// every texture the renderer attaches this way is a 2D texture
#define glFramebufferTexture(target, attachment, texture, level) \
    glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, texture, level)

#define glClearDepth glClearDepthf
#define glDepthRange glDepthRangef

// WebGL2 has no border clamping; the shaders that need a transparent outside test the
// coordinates themselves (REBIT_NO_BORDER_CLAMP)
#define GL_CLAMP_TO_BORDER GL_CLAMP_TO_EDGE

// fragment outputs get layout(location) in the shader source instead (OpenGLSupport.cpp)
#define glBindFragDataLocation(program, index, name) ((void)0)

// The DS's 16-bit colours (A1 B5 G5 R5, GL's 1_5_5_5_REV) as ES's RGB5_A1 (R5 G5 B5 A1)
// uploads, and back from an RGBA8 readback.
void rebitTexImage2D1555(GLenum target, GLint level, GLsizei width, GLsizei height, const void* data);
void rebitTexSubImage2D1555(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, const void* data);
void rebitTexImage3D1555(GLenum target, GLint level, GLsizei width, GLsizei height, GLsizei depth, const void* data);
void rebitTexSubImage3D1555(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height,
    GLsizei depth, const void* data);
void rebitReadPixels1555(GLint x, GLint y, GLsizei width, GLsizei height, void* data);

#endif
