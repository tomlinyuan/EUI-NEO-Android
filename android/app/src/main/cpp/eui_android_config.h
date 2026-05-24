#ifndef EUI_ANDROID_CONFIG_H
#define EUI_ANDROID_CONFIG_H

/* GLSL version string for Android (OpenGL ES 3.0) */
#define EUI_GLSL_VERSION "#version 300 es"

/* Fragment shader output declaration (GLES requires layout qualifier) */
#define EUI_FRAG_OUTPUT "layout(location = 0) out vec4 FragColor;"

/* Extension for fwidth() in GLES 3.0 */
#define EUI_FWIDTH_EXT "#extension GL_OES_standard_derivatives : enable"

#endif
