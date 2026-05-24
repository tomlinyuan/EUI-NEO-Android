/*
 * GLAD compatibility header for Android OpenGL ES 3.0.
 */
#ifndef GLAD_GLES_GLAD_H
#define GLAD_GLES_GLAD_H

#include <GLES3/gl3.h>
#include <GLES3/gl3platform.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GLADloadproc)(const char* name);

static inline int gladLoadGLLoader(GLADloadproc loadproc) {
    (void)loadproc;
    return 1;
}

static inline int gladLoadGL(void) {
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
