#include <GLFW/glfw3.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" int eui_android_main(void);
// Defined in android_app.cpp — called from nativeSetDarkMode JNI to relay
// Android's reported uiMode to the app's theme logic.
extern "C" void eui_android_set_system_dark_mode(int dark);

struct AndroidBridge {
    ANativeWindow* nativeWindow = nullptr;   // currently-bound surface
    ANativeWindow* pendingWindow = nullptr;  // new surface waiting to be bound
    AAssetManager* assetManager = nullptr;
    std::string fontsDir;
    std::atomic<bool> exitRequested{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> surfaceLost{false};    // set on surfaceDestroyed; cleared after native releases EGL
    std::atomic<bool> surfaceReady{false};   // set when a new surface is handed in
    int pendingWidth = 0;
    int pendingHeight = 0;
    std::atomic<bool> hasPendingResize{false};
    std::mutex surfaceMutex;
    std::condition_variable surfaceCv;

    struct TouchEvent { float x, y; int action; };
    std::mutex touchMutex;
    std::vector<TouchEvent> touchQueue;

    struct KeyEvent { int keyCode; int action; };
    std::mutex keyMutex;
    std::vector<KeyEvent> keyQueue;

    std::mutex charMutex;
    std::vector<unsigned int> charQueue;

    // IME state — accessed from native render thread and the JNI thread.
    JavaVM* jvm = nullptr;
    jobject activityRef = nullptr;
    jmethodID showKeyboardMethod = nullptr;
    jmethodID hideKeyboardMethod = nullptr;
    std::atomic<bool> imeVisible{false};
    std::atomic<bool> imePingedThisFrame{false};

    std::mutex clipboardMutex;
    std::string clipboard;

    static AndroidBridge& instance() {
        static AndroidBridge bridge;
        return bridge;
    }
};

struct WindowEGL {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig  config  = nullptr;
    int width  = 0;
    int height = 0;
};

struct GLFWwindow {
    ANativeWindow* nativeWindow = nullptr;
    WindowEGL egl;
    bool shouldClose = false;
    void* userPointer = nullptr;

    struct InputState {
        int keys[512] = {0};
        int mouseButtons[8] = {0};
        double cursorX = 0.0;
        double cursorY = 0.0;
        int inputModeCursor = GLFW_CURSOR_NORMAL;
        int inputModeStickyKeys = GLFW_FALSE;
        int inputModeStickyMouse = GLFW_FALSE;
    } input;

    GLFWwindowclosefun     closeCallback = nullptr;
    GLFWwindowsizefun      sizeCallback = nullptr;
    GLFWframebuffersizefun fbSizeCallback = nullptr;
    GLFWwindowposfun       posCallback = nullptr;
    GLFWkeyfun             keyCallback = nullptr;
    GLFWcharfun            charCallback = nullptr;
    GLFWcursorposfun       cursorPosCallback = nullptr;
    GLFWmousebuttonfun     mouseButtonCallback = nullptr;
    GLFWscrollfun          scrollCallback = nullptr;

    int currentCursorShape = GLFW_ARROW_CURSOR;
    double createTime = 0.0;
};

namespace {
    GLFWwindow*  g_currentWindow = nullptr;
    GLFWmonitor* g_primaryMonitor = nullptr;
    GLFWvidmode  g_videoMode = {1080, 1920, 8, 8, 8, 60};
    bool         g_glfwInitialized = false;
    double       g_startTime = 0.0;

    double nowSeconds() {
        auto now = std::chrono::steady_clock::now();
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()
        ) / 1e9;
    }

    int androidKeyToGlfw(int ak) {
        switch (ak) {
            case 67:  return GLFW_KEY_BACKSPACE;
            case 66:  return GLFW_KEY_ENTER;
            case 112: return GLFW_KEY_DELETE;
            case 19:  return GLFW_KEY_UP;
            case 20:  return GLFW_KEY_DOWN;
            case 21:  return GLFW_KEY_LEFT;
            case 22:  return GLFW_KEY_RIGHT;
            case 61:  return GLFW_KEY_TAB;
            case 111: return GLFW_KEY_ESCAPE;
            case 29:  return 'A';
            case 31:  return 'C';
            case 50:  return 'V';
            case 52:  return 'X';
            case 73:  return GLFW_KEY_BACKSLASH;
            case 122: return GLFW_KEY_HOME;
            case 123: return GLFW_KEY_END;
            default:  return 0;
        }
    }
}

int glfwInit(void) {
    if (g_glfwInitialized) return GLFW_TRUE;
    g_startTime = nowSeconds();
    g_glfwInitialized = true;
    return GLFW_TRUE;
}

void glfwTerminate(void) { g_glfwInitialized = false; }
void glfwInitHint(int, int) {}
void glfwDefaultWindowHints(void) {}
void glfwWindowHint(int, int) {}

GLFWwindow* glfwCreateWindow(int width, int height, const char*,
                              GLFWmonitor*, GLFWwindow*) {
    auto& bridge = AndroidBridge::instance();

    ANativeWindow* nw = nullptr;
    {
        std::lock_guard<std::mutex> lock(bridge.surfaceMutex);
        nw = bridge.nativeWindow;
    }
    if (!nw) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "createWindow: nativeWindow is null");
        return nullptr;
    }

    GLFWwindow* w = new GLFWwindow();
    w->nativeWindow = nw;
    w->createTime = nowSeconds();
    w->egl.width  = width  > 0 ? width  : 1080;
    w->egl.height = height > 0 ? height : 1920;

    ANativeWindow_setBuffersGeometry(nw, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "eglGetDisplay failed: 0x%x", eglGetError());
        delete w; return nullptr;
    }

    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "eglInitialize failed: 0x%x", eglGetError());
        delete w; return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "EUI", "EGL initialized: %d.%d", maj, min);

    // Widened from ES3_BIT to ES2|ES3 so software / older emulator EGL stacks
    // that only tag their window configs with ES2_BIT still match. We still
    // ask for an ES3 context below (ctxAttr) — most drivers will hand one out
    // even when the config itself is marked ES2.
    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint nCfg; EGLConfig cfg;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &nCfg) || nCfg == 0) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI",
            "eglChooseConfig: matched=%d err=0x%x", nCfg, eglGetError());
        eglTerminate(dpy); delete w; return nullptr;
    }
    EGLint cfgId = 0, cfgRenderable = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_CONFIG_ID, &cfgId);
    eglGetConfigAttrib(dpy, cfg, EGL_RENDERABLE_TYPE, &cfgRenderable);
    __android_log_print(ANDROID_LOG_INFO, "EUI",
        "chosen EGLConfig id=%d renderable=0x%x", cfgId, cfgRenderable);

    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI",
            "eglCreateContext(ES3) failed: 0x%x", eglGetError());
        eglTerminate(dpy); delete w; return nullptr;
    }

    EGLSurface sfc = eglCreateWindowSurface(dpy, cfg, nw, nullptr);
    if (sfc == EGL_NO_SURFACE) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI",
            "eglCreateWindowSurface failed: 0x%x", eglGetError());
        eglDestroyContext(dpy, ctx); eglTerminate(dpy); delete w; return nullptr;
    }

    w->egl.display = dpy; w->egl.config = cfg;
    w->egl.context = ctx;  w->egl.surface = sfc;

    if (!eglMakeCurrent(dpy, sfc, sfc, ctx)) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI",
            "eglMakeCurrent failed: 0x%x", eglGetError());
        eglDestroySurface(dpy, sfc); eglDestroyContext(dpy, ctx);
        eglTerminate(dpy); delete w; return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "EUI", "EGL context ready, GL_VERSION=%s",
                        (const char*)glGetString(GL_VERSION));

    g_currentWindow = w;
    return w;
}

void glfwDestroyWindow(GLFWwindow* w) {
    if (!w) return;
    if (g_currentWindow == w) {
        eglMakeCurrent(w->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        g_currentWindow = nullptr;
    }
    if (w->egl.surface != EGL_NO_SURFACE) eglDestroySurface(w->egl.display, w->egl.surface);
    if (w->egl.context != EGL_NO_CONTEXT) eglDestroyContext(w->egl.display, w->egl.context);
    if (w->egl.display != EGL_NO_DISPLAY) eglTerminate(w->egl.display);
    delete w;
}

int  glfwWindowShouldClose(GLFWwindow* w) {
    return (!w || w->shouldClose || AndroidBridge::instance().exitRequested.load()) ? 1 : 0;
}
void glfwSetWindowShouldClose(GLFWwindow* w, int v) { if (w) w->shouldClose = (v != 0); }
void glfwSetWindowTitle(GLFWwindow*, const char*) {}
void glfwSetWindowUserPointer(GLFWwindow* w, void* p) { if (w) w->userPointer = p; }
void* glfwGetWindowUserPointer(GLFWwindow* w) { return w ? w->userPointer : nullptr; }
void glfwSetWindowSize(GLFWwindow* w, int width, int height) { if (w) { w->egl.width = width; w->egl.height = height; } }
void glfwGetWindowSize(GLFWwindow* w, int* width, int* height) {
    if (w) { if (width) *width = w->egl.width; if (height) *height = w->egl.height; }
}
void glfwGetFramebufferSize(GLFWwindow* w, int* width, int* height) {
    if (w) { if (width) *width = w->egl.width; if (height) *height = w->egl.height; }
}
void glfwGetWindowPos(GLFWwindow*, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; }
void glfwSetWindowPos(GLFWwindow*, int, int) {}
void glfwGetWindowContentScale(GLFWwindow*, float* xs, float* ys) {
    if (xs) *xs = 2.0f; if (ys) *ys = 2.0f;
}
void glfwShowWindow(GLFWwindow*) {}
void glfwHideWindow(GLFWwindow*) {}
void glfwIconifyWindow(GLFWwindow*) {}
void glfwRestoreWindow(GLFWwindow*) {}
void glfwMaximizeWindow(GLFWwindow*) {}
void glfwFocusWindow(GLFWwindow*) {}

GLFWmonitor* glfwGetPrimaryMonitor(void) { return g_primaryMonitor; }
GLFWmonitor** glfwGetMonitors(int* c) { if (c) *c = 1; return &g_primaryMonitor; }
void glfwGetMonitorPos(GLFWmonitor*, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; }
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*) { return &g_videoMode; }
GLFWmonitor* glfwGetWindowMonitor(GLFWwindow*) { return g_primaryMonitor; }

GLFWwindow* glfwGetCurrentContext(void) { return g_currentWindow; }

void glfwMakeContextCurrent(GLFWwindow* w) {
    if (!w) {
        eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        g_currentWindow = nullptr;
        return;
    }
    eglMakeCurrent(w->egl.display, w->egl.surface, w->egl.surface, w->egl.context);
    g_currentWindow = w;
}

void glfwSwapBuffers(GLFWwindow* w) {
    if (w && w->egl.display != EGL_NO_DISPLAY)
        eglSwapBuffers(w->egl.display, w->egl.surface);
}

const char* glfwGetClipboardString(void* window) {
    (void)window;
    auto& bridge = AndroidBridge::instance();
    std::lock_guard<std::mutex> lock(bridge.clipboardMutex);
    return bridge.clipboard.empty() ? nullptr : bridge.clipboard.c_str();
}

void glfwSetClipboardString(void* window, const char* string) {
    (void)window;
    auto& bridge = AndroidBridge::instance();
    std::lock_guard<std::mutex> lock(bridge.clipboardMutex);
    bridge.clipboard = string ? string : "";
}

void glfwSetWindowIcon(GLFWwindow* window, int count, const GLFWimage* images) {
    (void)window; (void)count; (void)images;
    // no-op on Android
}

void glfwSwapInterval(int interval) {
    if (g_currentWindow && g_currentWindow->egl.display != EGL_NO_DISPLAY)
        eglSwapInterval(g_currentWindow->egl.display, interval);
}

int glfwGetInputMode(GLFWwindow* w, int mode) {
    if (!w) return 0;
    if (mode == GLFW_CURSOR) return w->input.inputModeCursor;
    if (mode == GLFW_STICKY_KEYS) return w->input.inputModeStickyKeys;
    if (mode == GLFW_STICKY_MOUSE_BUTTONS) return w->input.inputModeStickyMouse;
    return 0;
}
void glfwSetInputMode(GLFWwindow* w, int mode, int v) {
    if (!w) return;
    if (mode == GLFW_CURSOR) w->input.inputModeCursor = v;
    if (mode == GLFW_STICKY_KEYS) w->input.inputModeStickyKeys = v;
    if (mode == GLFW_STICKY_MOUSE_BUTTONS) w->input.inputModeStickyMouse = v;
}

int glfwGetKey(GLFWwindow* w, int key) {
    if (!w || key < 0 || key >= 512) return GLFW_RELEASE;
    return w->input.keys[key];
}
int glfwGetMouseButton(GLFWwindow* w, int btn) {
    if (!w || btn < 0 || btn >= 8) return GLFW_RELEASE;
    return w->input.mouseButtons[btn];
}
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
    if (w) { if (x) *x = w->input.cursorX; if (y) *y = w->input.cursorY; }
}

GLFWglproc glfwGetProcAddress(const char* name) {
    return reinterpret_cast<GLFWglproc>(eglGetProcAddress(name));
}

GLFWcursor* glfwCreateStandardCursor(int shape) {
    return reinterpret_cast<GLFWcursor*>(static_cast<intptr_t>(shape));
}
void glfwDestroyCursor(GLFWcursor*) {}
void glfwSetCursor(GLFWwindow* w, GLFWcursor* c) {
    if (w && c) w->currentCursorShape = static_cast<int>(reinterpret_cast<intptr_t>(c));
}

GLFWwindowclosefun glfwSetWindowCloseCallback(GLFWwindow* w, GLFWwindowclosefun cb) {
    if (!w) return nullptr; auto old = w->closeCallback; w->closeCallback = cb; return old;
}
GLFWwindowsizefun glfwSetWindowSizeCallback(GLFWwindow* w, GLFWwindowsizefun cb) {
    if (!w) return nullptr; auto old = w->sizeCallback; w->sizeCallback = cb; return old;
}
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow* w, GLFWframebuffersizefun cb) {
    if (!w) return nullptr; auto old = w->fbSizeCallback; w->fbSizeCallback = cb; return old;
}
GLFWwindowposfun glfwSetWindowPosCallback(GLFWwindow* w, GLFWwindowposfun cb) {
    if (!w) return nullptr; auto old = w->posCallback; w->posCallback = cb; return old;
}
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* w, GLFWkeyfun cb) {
    if (!w) return nullptr; auto old = w->keyCallback; w->keyCallback = cb; return old;
}
GLFWcharfun glfwSetCharCallback(GLFWwindow* w, GLFWcharfun cb) {
    if (!w) return nullptr; auto old = w->charCallback; w->charCallback = cb; return old;
}
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow* w, GLFWcursorposfun cb) {
    if (!w) return nullptr; auto old = w->cursorPosCallback; w->cursorPosCallback = cb; return old;
}
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun cb) {
    if (!w) return nullptr; auto old = w->mouseButtonCallback; w->mouseButtonCallback = cb; return old;
}
GLFWscrollfun glfwSetScrollCallback(GLFWwindow* w, GLFWscrollfun cb) {
    if (!w) return nullptr; auto old = w->scrollCallback; w->scrollCallback = cb; return old;
}

double glfwGetTime(void) { return nowSeconds() - g_startTime; }

void glfwPostEmptyEvent(void) {
    /* No-op on Android: the main loop polls via processAndroidEvents */
}

static void processAndroidEvents(GLFWwindow* w) {
    auto& bridge = AndroidBridge::instance();

    if (bridge.hasPendingResize.exchange(false)) {
        std::lock_guard<std::mutex> lock(bridge.surfaceMutex);
        w->egl.width  = bridge.pendingWidth;
        w->egl.height = bridge.pendingHeight;
        if (w->fbSizeCallback) w->fbSizeCallback(w, bridge.pendingWidth, bridge.pendingHeight);
        if (w->sizeCallback)   w->sizeCallback(w, bridge.pendingWidth, bridge.pendingHeight);
    }

    {
        std::lock_guard<std::mutex> lock(bridge.touchMutex);
        auto& queue = bridge.touchQueue;

        // Drain all MOVE events first — they only update cursor position and
        // don't affect the press state.
        for (auto it = queue.begin(); it != queue.end(); ) {
            if (it->action == 2) {  // MOVE
                w->input.cursorX = it->x;
                w->input.cursorY = it->y;
                if (w->cursorPosCallback) w->cursorPosCallback(w, it->x, it->y);
                it = queue.erase(it);
            } else {
                ++it;
            }
        }

        // Now process at most ONE press-state transition per frame. The runtime
        // polls mouseButton state once per frame; if DOWN and UP both arrive in
        // the same batch they'd both flip the same bit and the runtime would
        // see no press transition at all — quick taps would be lost. Spreading
        // them across frames guarantees the runtime sees pressedThisFrame on
        // one frame and releasedThisFrame on the next.
        if (!queue.empty()) {
            auto& ev = queue.front();
            w->input.cursorX = ev.x;
            w->input.cursorY = ev.y;
            if (w->cursorPosCallback) w->cursorPosCallback(w, ev.x, ev.y);
            if (ev.action == 1) {
                w->input.mouseButtons[GLFW_MOUSE_BUTTON_LEFT] = GLFW_PRESS;
                // Treat any user press as a fresh gesture and invalidate our
                // cached IME-visible flag. Android's IME window swallows the
                // back key internally to dismiss itself, so we never get a
                // callback when the keyboard goes away by user action — our
                // flag would stay true and subsequent set_cursor_rect pings
                // would skip the show call. Clearing on every press lets the
                // next focused-input ping re-issue showSoftInput.
                bridge.imeVisible.store(false);
                if (w->mouseButtonCallback)
                    w->mouseButtonCallback(w, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
            } else if (ev.action == 0) {
                w->input.mouseButtons[GLFW_MOUSE_BUTTON_LEFT] = GLFW_RELEASE;
                if (w->mouseButtonCallback)
                    w->mouseButtonCallback(w, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
            }
            queue.erase(queue.begin());
        }
    }

    {
        std::lock_guard<std::mutex> lock(bridge.keyMutex);
        for (auto& ev : bridge.keyQueue) {
            int gk = androidKeyToGlfw(ev.keyCode);
            if (gk != 0 && gk < 512) {
                int act = (ev.action == 1) ? GLFW_PRESS : GLFW_RELEASE;
                w->input.keys[gk] = act;
                if (w->keyCallback) w->keyCallback(w, gk, 0, act, 0);
            }
        }
        bridge.keyQueue.clear();
    }

    {
        std::lock_guard<std::mutex> lock(bridge.charMutex);
        for (unsigned int codepoint : bridge.charQueue) {
            if (w->charCallback && codepoint >= 0x20 && codepoint != 0x7F) {
                w->charCallback(w, codepoint);
            }
        }
        bridge.charQueue.clear();
    }
}

void glfwPollEvents(void) {
    if (g_currentWindow) processAndroidEvents(g_currentWindow);
}

void glfwWaitEvents(void) {
    if (g_currentWindow) processAndroidEvents(g_currentWindow);
    // If there are still queued touch state transitions, don't sleep — we
    // process one transition per frame to keep press/release distinguishable,
    // and we want those frames to fire back-to-back.
    auto& bridge = AndroidBridge::instance();
    {
        std::lock_guard<std::mutex> lock(bridge.touchMutex);
        if (!bridge.touchQueue.empty()) return;
    }
    struct timespec ts = {0, 16666667};
    nanosleep(&ts, nullptr);
}

void glfwWaitEventsTimeout(double timeout) {
    if (g_currentWindow) processAndroidEvents(g_currentWindow);
    if (timeout > 0.0) {
        struct timespec ts;
        ts.tv_sec  = static_cast<time_t>(timeout);
        ts.tv_nsec = static_cast<long>((timeout - ts.tv_sec) * 1e9);
        nanosleep(&ts, nullptr);
    }
}

const char* glfwGetError(int*) { return nullptr; }

// ================================================================
// Surface lifecycle helpers used by the native main loop
// Together with the JNI nativeSurfaceCreated/Destroyed they allow the
// native thread to survive across Activity foreground/background cycles.
// ================================================================

extern "C" bool eui_android_exit_requested(void) {
    return AndroidBridge::instance().exitRequested.load();
}

extern "C" bool eui_android_surface_lost(void) {
    return AndroidBridge::instance().surfaceLost.load();
}

extern "C" void eui_android_release_egl(GLFWwindow* w) {
    if (!w) return;
    // Full teardown: destroy EGL surface, context, display, and the GLFWwindow
    // struct itself. The Runtime's static SharedResources maps are keyed by
    // GLFWwindow*, so when we create a new window after this, those maps get
    // fresh entries — no risk of stale GLuint handles from the old context.
    glfwDestroyWindow(w);

    auto& bridge = AndroidBridge::instance();
    ANativeWindow* released = nullptr;
    {
        std::lock_guard<std::mutex> lock(bridge.surfaceMutex);
        released = bridge.nativeWindow;
        bridge.nativeWindow = nullptr;
    }
    if (released) ANativeWindow_release(released);
    bridge.surfaceLost.store(false);
    // Also clear surfaceReady — it was set by the previous nativeMain /
    // nativeSurfaceCreated call, and without resetting it here wait_for_surface
    // would return immediately (predicate already true) the next time we go
    // around the outer loop, tight-looping glfwCreateWindow until the activity
    // is destroyed.
    bridge.surfaceReady.store(false);
    // Android dismisses the soft keyboard whenever the surface goes away. Clear
    // our cached IME-visible flag so the next focused input triggers a fresh
    // showSoftInput when the surface comes back — without this, the C++ side
    // thinks the keyboard is still up and skips the show call forever.
    bridge.imeVisible.store(false);
    bridge.surfaceCv.notify_all();
    __android_log_print(ANDROID_LOG_INFO, "EUI", "Destroyed window + EGL");
}

// Move the pending ANativeWindow (handed in by nativeSurfaceCreated) into the
// nativeWindow slot so the next glfwCreateWindow call picks it up. Returns
// true if a pending surface was promoted.
extern "C" bool eui_android_promote_pending_window(void) {
    auto& bridge = AndroidBridge::instance();
    std::lock_guard<std::mutex> lock(bridge.surfaceMutex);
    if (!bridge.pendingWindow) return false;
    if (bridge.nativeWindow) {
        ANativeWindow_release(bridge.nativeWindow);
    }
    bridge.nativeWindow = bridge.pendingWindow;
    bridge.pendingWindow = nullptr;
    bridge.surfaceReady.store(false);
    return true;
}

// ================================================================
// IME (soft keyboard) integration
// ================================================================

namespace {

void callJavaVoidMethod(jmethodID method) {
    auto& bridge = AndroidBridge::instance();
    if (!bridge.jvm || !bridge.activityRef || !method) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (bridge.jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (bridge.jvm->AttachCurrentThread(&env, nullptr) != 0) {
            return;
        }
        attached = true;
    }
    if (env) {
        env->CallVoidMethod(bridge.activityRef, method);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
    if (attached) bridge.jvm->DetachCurrentThread();
}

} // namespace

// Called by the runtime each frame while a focusable element with an IME
// rect is focused. We treat each call as a "keep IME visible" ping; if a
// frame passes without any ping while IME was visible, we hide it.
extern "C" void eui_ime_set_cursor_rect(GLFWwindow* window, double x, double y, double width, double height) {
    (void)window; (void)x; (void)y; (void)width; (void)height;
    auto& bridge = AndroidBridge::instance();
    bridge.imePingedThisFrame.store(true);
    if (!bridge.imeVisible.load()) {
        bridge.imeVisible.store(true);
        callJavaVoidMethod(bridge.showKeyboardMethod);
    }
}

// Called once per frame by the native main loop. Hides IME if focus has
// moved away (no set_cursor_rect ping this frame) while keyboard is visible.
extern "C" void eui_android_poll_ime_frame(void) {
    auto& bridge = AndroidBridge::instance();
    const bool pinged = bridge.imePingedThisFrame.exchange(false);
    if (!pinged && bridge.imeVisible.load()) {
        bridge.imeVisible.store(false);
        callJavaVoidMethod(bridge.hideKeyboardMethod);
    }
}

// Block until a new surface is handed in via nativeSurfaceCreated, or until
// the activity is being destroyed. Returns false on real exit.
extern "C" bool eui_android_wait_for_surface(void) {
    auto& bridge = AndroidBridge::instance();
    std::unique_lock<std::mutex> lock(bridge.surfaceMutex);
    bridge.surfaceCv.wait(lock, [&] {
        return bridge.surfaceReady.load() || bridge.exitRequested.load();
    });
    return !bridge.exitRequested.load();
}

// Bind the pending ANativeWindow to a fresh EGLSurface on the existing
// EGLContext. The context preserves all GL state (shaders, VAOs, textures)
// from before the surface was lost, so nothing needs to be re-uploaded.
extern "C" bool eui_android_acquire_egl(GLFWwindow* w) {
    // Kept as a no-op symbol for ABI compatibility; the new flow uses
    // glfwCreateWindow + eui_android_promote_pending_window instead.
    (void)w;
    return false;
}

// ================================================================
// JNI entry points
// Font asset extraction helpers
static std::string getAppFilesDir(JNIEnv* env, jobject activity) {
    jclass activityClass = env->GetObjectClass(activity);
    if (!activityClass || env->ExceptionCheck()) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "GetObjectClass failed");
        return "";
    }
    jmethodID getFilesDir = env->GetMethodID(activityClass, "getFilesDir", "()Ljava/io/File;");
    if (!getFilesDir) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "getFilesDir method not found");
        env->DeleteLocalRef(activityClass);
        return "";
    }
    jobject fileObj = env->CallObjectMethod(activity, getFilesDir);
    if (!fileObj || env->ExceptionCheck()) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "getFilesDir() call failed");
        env->DeleteLocalRef(activityClass);
        return "";
    }
    jclass fileClass = env->GetObjectClass(fileObj);
    jmethodID getAbsolutePath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    if (!getAbsolutePath) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "getAbsolutePath method not found");
        env->DeleteLocalRef(fileObj);
        env->DeleteLocalRef(activityClass);
        return "";
    }
    jstring pathStr = (jstring)env->CallObjectMethod(fileObj, getAbsolutePath);
    if (!pathStr || env->ExceptionCheck()) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, "EUI", "getAbsolutePath() call failed");
        env->DeleteLocalRef(fileObj);
        env->DeleteLocalRef(activityClass);
        return "";
    }
    const char* pathChars = env->GetStringUTFChars(pathStr, nullptr);
    std::string result(pathChars);
    env->ReleaseStringUTFChars(pathStr, pathChars);
    env->DeleteLocalRef(pathStr);
    env->DeleteLocalRef(fileObj);
    env->DeleteLocalRef(activityClass);
    __android_log_print(ANDROID_LOG_INFO, "EUI", "App files dir: %s", result.c_str());
    return result;
}

static bool extractAssetToFile(AAssetManager* mgr, const char* assetName, const std::string& destDir) {
    AAsset* asset = AAssetManager_open(mgr, assetName, AASSET_MODE_STREAMING);
    if (!asset) {
        __android_log_print(ANDROID_LOG_WARN, "EUI", "Asset not found: %s", assetName);
        return false;
    }
    off_t length = AAsset_getLength(asset);
    if (length <= 0) {
        __android_log_print(ANDROID_LOG_WARN, "EUI", "Asset empty: %s", assetName);
        AAsset_close(asset);
        return false;
    }
    std::string destPath = destDir + "/" + assetName;
    std::ofstream out(destPath, std::ios::binary);
    if (!out) {
        __android_log_print(ANDROID_LOG_WARN, "EUI", "Cannot write: %s", destPath.c_str());
        AAsset_close(asset);
        return false;
    }
    std::vector<char> buf(65536);
    off_t remaining = length;
    while (remaining > 0) {
        int toRead = (remaining < (off_t)buf.size()) ? (int)remaining : (int)buf.size();
        int n = AAsset_read(asset, buf.data(), toRead);
        if (n <= 0) break;
        out.write(buf.data(), n);
        remaining -= n;
    }
    out.close();
    AAsset_close(asset);
    __android_log_print(ANDROID_LOG_INFO, "EUI", "Extracted: %s -> %s (%ld bytes)", assetName, destPath.c_str(), (long)length);
    return true;
}

static void extractFontAssets(JNIEnv* env, jobject activity, AndroidBridge& bridge) {
    if (!bridge.assetManager) {
        __android_log_print(ANDROID_LOG_WARN, "EUI", "No AssetManager, skipping font extraction");
        return;
    }
    std::string filesDir = getAppFilesDir(env, activity);
    bridge.fontsDir = filesDir;

    const char* fonts[] = {
        "JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf",
        "Font Awesome 7 Free-Solid-900.otf"
    };

    for (const char* fontName : fonts) {
        if (extractAssetToFile(bridge.assetManager, fontName, bridge.fontsDir)) {
            __android_log_print(ANDROID_LOG_INFO, "EUI", "Font extracted: %s", fontName);
        }
    }
}

extern "C" const char* eui_android_text_font_path(void) {
    static std::string path;
    const auto& dir = AndroidBridge::instance().fontsDir;
    if (!dir.empty()) {
        path = dir + "/JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf";
    }
    if (path.empty()) {
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "EUI", "Text font path: %s", path.c_str());
    return path.c_str();
}

extern "C" const char* eui_android_icon_font_path(void) {
    static std::string path;
    const auto& dir = AndroidBridge::instance().fontsDir;
    if (!dir.empty()) {
        path = dir + "/Font Awesome 7 Free-Solid-900.otf";
    }
    if (path.empty()) {
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "EUI", "Icon font path: %s", path.c_str());
    return path.c_str();
}

// Returns the app's private files directory (where extractFontAssets writes
// to). Apps can use this to persist their own state files alongside the fonts.
extern "C" const char* eui_android_files_dir(void) {
    const auto& dir = AndroidBridge::instance().fontsDir;
    return dir.empty() ? nullptr : dir.c_str();
}


extern "C" {

JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeMain(JNIEnv* env, jobject thiz,
                                         jobject surface, jobject assetManager) {
    auto& bridge = AndroidBridge::instance();
    {
        std::lock_guard<std::mutex> lock(bridge.surfaceMutex);
        bridge.nativeWindow = ANativeWindow_fromSurface(env, surface);
        bridge.assetManager = AAssetManager_fromJava(env, assetManager);
        bridge.surfaceReady.store(true);
        bridge.surfaceLost.store(false);
        bridge.exitRequested.store(false);
        bridge.imeVisible.store(false);

        // Re-bind activity ref every time, even if it's already set: when the
        // user presses back to finish the activity and re-launches, Android
        // creates a NEW Activity instance and a fresh nativeMain call comes in.
        // The static `bridge` survives across activity instances, so the cached
        // global ref would point to the now-destroyed previous activity and
        // every native→Java callback (showKeyboard etc.) would silently fail.
        if (!bridge.jvm) {
            env->GetJavaVM(&bridge.jvm);
        }
        if (bridge.activityRef) {
            env->DeleteGlobalRef(bridge.activityRef);
        }
        bridge.activityRef = env->NewGlobalRef(thiz);
        jclass cls = env->GetObjectClass(thiz);
        bridge.showKeyboardMethod = env->GetMethodID(cls, "onNativeShowKeyboard", "()V");
        bridge.hideKeyboardMethod = env->GetMethodID(cls, "onNativeHideKeyboard", "()V");
        env->DeleteLocalRef(cls);
    }
    bridge.surfaceCv.notify_all();
    // Extract font assets to filesystem so text.cpp can load them
    extractFontAssets(env, thiz, bridge);
    eui_android_main();
}


JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeResize(JNIEnv*, jobject, jint w, jint h) {
    auto& bridge = AndroidBridge::instance();
    std::lock_guard<std::mutex> lock(bridge.surfaceMutex);
    bridge.pendingWidth = w; bridge.pendingHeight = h;
    bridge.hasPendingResize.store(true);
}


JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeTouchEvent(JNIEnv*, jobject,
                                               jfloat x, jfloat y, jint action) {
    auto& bridge = AndroidBridge::instance();
    std::lock_guard<std::mutex> lock(bridge.touchMutex);
    bridge.touchQueue.push_back({x, y, action});
}


JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeKeyEvent(JNIEnv*, jobject, jint code, jint action) {
    auto& bridge = AndroidBridge::instance();
    std::lock_guard<std::mutex> lock(bridge.keyMutex);
    bridge.keyQueue.push_back({code, action});
}


// Receives Unicode codepoints typed via the soft keyboard. Forwarded to the
// GLFW char callback so the runtime treats them as text input.
JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeTextInput(JNIEnv*, jobject, jint codepoint) {
    auto& bridge = AndroidBridge::instance();
    std::lock_guard<std::mutex> lock(bridge.charMutex);
    bridge.charQueue.push_back(static_cast<unsigned int>(codepoint));
}


JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeRequestExit(JNIEnv*, jobject) {
    auto& bridge = AndroidBridge::instance();
    bridge.exitRequested.store(true);
    bridge.surfaceCv.notify_all();
}


// Called whenever the SurfaceView surface is destroyed (returning to home,
// app switcher, screen off, etc.). Signals the native loop to release its
// EGL context+surface and waits up to 2s for that to complete before
// returning to Android — otherwise the system may tear down the underlying
// Surface while EGL is still using it.
JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeSurfaceDestroyed(JNIEnv*, jobject) {
    auto& bridge = AndroidBridge::instance();
    bridge.surfaceLost.store(true);
    bridge.surfaceCv.notify_all();

    std::unique_lock<std::mutex> lock(bridge.surfaceMutex);
    bridge.surfaceCv.wait_for(lock, std::chrono::milliseconds(2000), [&] {
        return !bridge.surfaceLost.load() || bridge.exitRequested.load();
    });
}


// Called on subsequent surface re-creations (NOT the first one, which goes
// through nativeMain). Hands the new ANativeWindow to the native loop, which
// will rebuild its EGL state and resume rendering on the next iteration.
JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeSurfaceCreated(JNIEnv* env, jobject, jobject surface) {
    auto& bridge = AndroidBridge::instance();
    ANativeWindow* nw = ANativeWindow_fromSurface(env, surface);
    {
        std::lock_guard<std::mutex> lock(bridge.surfaceMutex);
        if (bridge.pendingWindow) {
            ANativeWindow_release(bridge.pendingWindow);
        }
        bridge.pendingWindow = nw;
        bridge.surfaceReady.store(true);
    }
    bridge.surfaceCv.notify_all();
}


JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativePause(JNIEnv*, jobject) {
    auto& bridge = AndroidBridge::instance();
    bridge.paused.store(true);
    // The system dismisses the soft keyboard on pause; clear our flag so the
    // next focused input re-triggers showSoftInput when we resume.
    bridge.imeVisible.store(false);
}


JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeResume(JNIEnv*, jobject) {
    AndroidBridge::instance().paused.store(false);
}

// Reported by Java from Configuration.uiMode whenever the system theme
// changes (onCreate + onConfigurationChanged). The app code reads this for
// its "auto" theme mode.
JNIEXPORT void JNICALL
Java_com_eui_neo_EuiActivity_nativeSetDarkMode(JNIEnv*, jobject, jboolean dark) {
    eui_android_set_system_dark_mode(dark ? 1 : 0);
}

} // extern "C"
