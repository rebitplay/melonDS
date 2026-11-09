// See rebit_gl.h.
#include "rebit_gl.h"

#include <vector>

#include <emscripten.h>

EM_JS(void, rebit_gl_color_maski, (unsigned index, int r, int g, int b, int a), {
    const context = GL.currentContext;
    const gl = context && context.GLctx;
    if (!gl) return;
    if (context.rebitDrawBuffersIndexed === undefined)
        context.rebitDrawBuffersIndexed = gl.getExtension("OES_draw_buffers_indexed") || null;
    const ext = context.rebitDrawBuffersIndexed;
    if (ext) ext.colorMaskiOES(index, !!r, !!g, !!b, !!a);
    else if (index === 0) gl.colorMask(!!r, !!g, !!b, !!a);
});

EM_JS(int, rebit_gl_has_draw_buffers_indexed, (), {
    const context = GL.currentContext;
    const gl = context && context.GLctx;
    return gl && gl.getExtension("OES_draw_buffers_indexed") ? 1 : 0;
});

bool rebitHasDrawBuffersIndexed()
{
    return rebit_gl_has_draw_buffers_indexed() != 0;
}

void rebitColorMaski(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    rebit_gl_color_maski(buf, r, g, b, a);
}

namespace
{
std::vector<uint16_t> Scratch;

// A1 B5 G5 R5 -> R5 G5 B5 A1
const uint16_t* To5551(const void* data, size_t count)
{
    if (!data)
        return nullptr;
    Scratch.resize(count);
    const uint16_t* in = static_cast<const uint16_t*>(data);
    for (size_t i = 0; i < count; i++)
    {
        const uint16_t c = in[i];
        Scratch[i] = ((c & 0x1F) << 11) | (((c >> 5) & 0x1F) << 6) | (((c >> 10) & 0x1F) << 1) | (c >> 15);
    }
    return Scratch.data();
}
}

void rebitTexImage2D1555(GLenum target, GLint level, GLsizei width, GLsizei height, const void* data)
{
    glTexImage2D(target, level, GL_RGB5_A1, width, height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1,
        To5551(data, (size_t)width * height));
}

void rebitTexSubImage2D1555(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, const void* data)
{
    glTexSubImage2D(target, level, x, y, width, height, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1,
        To5551(data, (size_t)width * height));
}

void rebitTexImage3D1555(GLenum target, GLint level, GLsizei width, GLsizei height, GLsizei depth, const void* data)
{
    glTexImage3D(target, level, GL_RGB5_A1, width, height, depth, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1,
        To5551(data, (size_t)width * height * depth));
}

void rebitTexSubImage3D1555(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height,
    GLsizei depth, const void* data)
{
    glTexSubImage3D(target, level, x, y, z, width, height, depth, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1,
        To5551(data, (size_t)width * height * depth));
}

void rebitReadPixels1555(GLint x, GLint y, GLsizei width, GLsizei height, void* data)
{
    static std::vector<uint8_t> rgba;
    rgba.resize((size_t)width * height * 4);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    uint16_t* out = static_cast<uint16_t*>(data);
    for (size_t i = 0; i < (size_t)width * height; i++)
    {
        const uint8_t* p = &rgba[i * 4];
        out[i] = (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | ((p[3] >= 128) << 15);
    }
}
