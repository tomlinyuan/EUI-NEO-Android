#include <glad/glad.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include "app/app.h"
#include "core/async.h"
#include "core/dsl_runtime.h"
#include "core/event.h"
#include "core/network.h"
#include "core/platform.h"
#include "core/text.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <android/log.h>

// Surface lifecycle helpers provided by glfw_shim
extern "C" bool eui_android_exit_requested(void);
extern "C" bool eui_android_surface_lost(void);
extern "C" void eui_android_release_egl(GLFWwindow* w);
extern "C" bool eui_android_wait_for_surface(void);
extern "C" bool eui_android_promote_pending_window(void);
extern "C" void eui_android_poll_ime_frame(void);

struct WindowState {
    bool needsRender = true;
    int renderedFrames = 0;
    double lastTitleUpdate = 0.0;
    double nextFrameTime = 0.0;
    double frameInterval = 1.0 / 60.0;
    double lastFrameRateLimit = 0.0;
    double lastRefreshRateUpdate = 0.0;
};

static float getDpiScale(GLFWwindow* window) {
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window, &sx, &sy);
    return (sx + sy) * 0.5f;
}

static float getPointerScale(GLFWwindow* window) {
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    if (ww <= 0 || wh <= 0) return 1.0f;
    return ((float)fw / (float)ww + (float)fh / (float)wh) * 0.5f;
}

static GLFWmonitor* getWindowMonitor(GLFWwindow* w) {
    if (GLFWmonitor* m = glfwGetWindowMonitor(w)) return m;
    return glfwGetPrimaryMonitor();
}

static double getWindowRefreshRate(GLFWwindow* w) {
    GLFWmonitor* m = getWindowMonitor(w);
    const GLFWvidmode* mode = m ? glfwGetVideoMode(m) : nullptr;
    return (mode && mode->refreshRate > 0) ? (double)mode->refreshRate : 60.0;
}

static void updateFrameInterval(GLFWwindow* w, WindowState& ws, double now, bool force = false) {
    double limit = app::frameRateLimit();
    if (!force && limit == ws.lastFrameRateLimit && now - ws.lastRefreshRateUpdate < 0.5) return;
    double rr = std::clamp(getWindowRefreshRate(w), 30.0, 500.0);
    if (limit > 0.0) rr = std::min(rr, limit);
    ws.frameInterval = 1.0 / std::max(1.0, rr);
    ws.lastFrameRateLimit = limit;
    ws.lastRefreshRateUpdate = now;
}

// Run one window's lifetime: create a fresh GLFWwindow against the current
// bridge.nativeWindow, run frames until surface is lost or we're exiting,
// then tear everything down. Returns true to continue the outer loop, false
// if we should exit entirely.
static bool runWindowSession() {
    GLFWwindow* window = glfwCreateWindow(1080, 1920, "EUI-NEO", nullptr, nullptr);
    if (!window) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "glfwCreateWindow failed");
        return !eui_android_exit_requested();
    }

    app::initialize(window);

    WindowState ws;
    double lastFrameTime = glfwGetTime();
    ws.nextFrameTime = lastFrameTime;
    ws.needsRender = true;
    int fbW = 0, fbH = 0;

    // Inner loop: render frames while the surface is alive.
    while (!eui_android_exit_requested() && !eui_android_surface_lost()) {
        glfwPollEvents();
        if (eui_android_surface_lost() || eui_android_exit_requested()) break;

        double currentTime = glfwGetTime();
        updateFrameInterval(window, ws, currentTime);
        float deltaSeconds = (float)(currentTime - lastFrameTime);
        lastFrameTime = currentTime;

        glfwGetFramebufferSize(window, &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0) {
            ws.needsRender = true;
            glfwWaitEvents();
            ws.nextFrameTime = glfwGetTime();
            lastFrameTime = ws.nextFrameTime;
            continue;
        }

        float dpiScale = getDpiScale(window);
        float ptrScale = getPointerScale(window);
        bool externalReady = core::network::consumeAnyTextReady() || core::async::dispatchReady();

        if (app::update(window, deltaSeconds, fbW, fbH, dpiScale, ptrScale, externalReady, true)) {
            ws.needsRender = true;
        }

        // After update (which may move focus on/off an input), tell the JNI
        // bridge whether to keep/hide the soft keyboard.
        eui_android_poll_ime_frame();

        if (ws.needsRender) {
            app::render(fbW, fbH, dpiScale);
            glfwSwapBuffers(window);
            ws.needsRender = false;
        }

        if (app::isAnimating()) {
            double now = glfwGetTime();
            ws.nextFrameTime += ws.frameInterval;
            if (ws.nextFrameTime <= now || ws.nextFrameTime > now + ws.frameInterval * 2.0)
                ws.nextFrameTime = now + ws.frameInterval;
            glfwPollEvents();
        } else {
            glfwWaitEvents();
            ws.nextFrameTime = glfwGetTime();
        }
    }

    // Surface lost or app exiting — full teardown of this window. While the
    // EGL context is still current, drop the runtime's GL handles so the
    // shared-resource maps (keyed by GLFWwindow*) won't keep leaked GLuints.
    app::releaseGraphicsResources();
    core::releaseInputQueue(window);
    eui_android_release_egl(window);

    return !eui_android_exit_requested();
}

extern "C" int eui_android_main(void) {
    // bridge.nativeWindow was already set by the nativeMain JNI for the first
    // session. Subsequent sessions get their surface from promote_pending_window
    // after wait_for_surface returns.
    while (!eui_android_exit_requested()) {
        if (!runWindowSession()) break;

        if (!eui_android_wait_for_surface()) break;
        if (!eui_android_promote_pending_window()) {
            // wait_for_surface returned true without exit, so there should be
            // a pending window. If not, loop and retry.
            continue;
        }
    }

    app::shutdown();
    glfwTerminate();
    return 0;
}
