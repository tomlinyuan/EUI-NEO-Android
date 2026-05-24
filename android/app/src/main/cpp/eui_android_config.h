#ifndef EUI_ANDROID_CONFIG_H
#define EUI_ANDROID_CONFIG_H

/* Shader source prelude for Android (OpenGL ES 3.0).
 *
 * GLES rejects "#version 330 core" (desktop GLSL) and additionally requires
 * an explicit default precision for fragment shaders. Vertex shaders default
 * to highp, so this prelude is safe to prepend to both stages.
 *
 * Upstream shader sources in core/primitive.h and core/text.cpp consume this
 * macro via an "#ifndef EUI_SHADER_PRELUDE" guard so the desktop build
 * silently falls back to "#version 330 core\n".
 */
#define EUI_SHADER_PRELUDE \
    "#version 300 es\n" \
    "precision highp float;\n"

#endif
