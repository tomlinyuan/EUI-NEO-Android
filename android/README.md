# EUI-NEO · Android Port

把 [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) 这个 C++17 桌面 UI 框架完整移植到 Android,通过 EGL + GLESv3 渲染,Kotlin 做最小化 Activity 外壳,核心代码与上游共用。

> **简介**:原版 EUI-NEO 是一个基于 OpenGL + GLFW 的跨平台 C++ UI 框架(Windows / Linux / macOS)。这个 fork 给它加了 Android 后端,组件、动画、文字、布局、主题等全部能用,提供完整的 demo App(侧边栏 + 6 个示例页面),并解决了从桌面到移动端涉及的所有生命周期、输入法、触屏交互、字体加载等问题。

---

## 📱 功能特性

- ✅ **完整组件库**:按钮 / 复选框 / 单选 / 开关 / 滑块 / 进度 / 文本输入 / 分段 / 步进 / 标签页 / 下拉框 / 日期 / 时间 / 颜色选择 / 对话框 / Toast / 折线图 / 柱状图 / 饼图 / 模拟时钟
- ✅ **主题系统**:浅色 / 深色 / 跟随系统三档,系统主题切换实时响应
- ✅ **软键盘集成**:点击输入框自动弹出系统 IME,支持中英文字符与组合输入
- ✅ **触屏手势**:点击 / 滚动 / 拖动 / 滑块拖拽,快速点击不丢失
- ✅ **生命周期**:home 键 / 返回键 / Activity 重建 / 屏幕旋转 / 系统主题切换全部正确处理
- ✅ **状态持久化**:所有用户选择(开关、滑块、单选、输入文本、主题等)自动保存到私有目录,下次启动恢复
- ✅ **中文渲染**:打包 SDF 字体,任意缩放清晰
- ✅ **图表动画**:折线 / 柱状图数据随时间波动,饼图渲染同步
- ✅ **模拟时钟**:每秒实时走动的指针,主题感知

---

## 🚀 快速开始

### 环境要求

- Android Studio Hedgehog (2023.1.1) 或更新
- Android SDK API 26+ 作为最低目标(代码用了部分 API 26 才有的特性)
- NDK 25.1+ 或更新
- CMake 3.22+

### 编译运行

```bash
git clone <your-repo-url>
cd EUI-NEO/android
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

或者直接在 Android Studio 里打开 `android/` 目录,Build → Make Project → Run。

### 第一次启动你会看到

- 左边 **92px 宽的侧边栏**:5 个图标按钮(控件 / 输入 / 选择 / 图表 / 时钟 / 关于),带跟随选中项滑动的 accent 条
- 右边**滚动内容区**,按当前页显示对应组件
- 点击"输入"页的文本框 → 系统软键盘弹出 → 输入中文 / 英文都能正确显示
- 控件页的"自动"单选默认选中 → 主题跟系统;切到"浅色"/"深色"立即生效
- "选择"页的主题色按钮 → 打开取色器 → 改色后整个 App 的 accent 跟着变(包括侧边栏滑条)

---

## 📁 项目结构

```
EUI-NEO/
├── core/                              # 原版核心,基本不动
│   ├── dsl.h, dsl_runtime.h           # DSL + Runtime
│   ├── primitive.h, text.{h,cpp}      # GL 图元、SDF 文字
│   ├── event.h                        # 输入事件
│   ├── ime_bridge.{h,c}               # 跨平台 IME 桥(我们加了 Android 分支)
│   └── ...
├── components/                        # 原版组件,基本不动(只改了 dropdown 和 scrollview)
│   ├── button.h, checkbox.h, ...
│   ├── dropdown.h                     # ⚠️ 改了 rootHeight 逻辑
│   └── scrollview.h                   # ⚠️ 加了触屏 drag 支持
├── app/                               # 原版桌面 demo,仅参考
│   ├── gallery.cpp                    # 桌面侧 gallery,我们参考它写了 Android 版
│   └── clock.cpp                      # 桌面侧时钟,我们参考它的 composeAnalogClock
└── android/                           # ★ Android 移植所有新代码
    ├── build.gradle.kts, settings.gradle.kts
    ├── app/
    │   ├── build.gradle.kts
    │   ├── CMakeLists.txt             # NDK 构建脚本
    │   └── src/main/
    │       ├── AndroidManifest.xml
    │       ├── assets/                # 打包字体
    │       │   ├── JingNanJunJunTi-...-Bold-2.ttf
    │       │   └── Font Awesome 7 Free-Solid-900.otf
    │       ├── java/com/eui/neo/
    │       │   └── EuiActivity.kt     # ★ Kotlin 入口 Activity
    │       └── cpp/
    │           ├── android_main.cpp   # ★ Android 主循环
    │           ├── android_app.cpp    # ★ 移动版 Demo Compose
    │           ├── eui_android_config.h  # GLES / 编译选项
    │           ├── glad_gles/glad/glad.h # GLES no-op stub(GLES 静态链接)
    │           └── glfw_shim/
    │               └── glfw_shim.cpp  # ★ GLFW → Android NDK shim
    └── README.md                      # 本文档
```

---

## 🏗️ 架构与移植说明

### 原版架构

```
┌─────────────────────────────────────────────┐
│ main.cpp                                     │
│  ├─ glfwInit + glfwCreateWindow             │
│  ├─ app::initialize + 主循环                │
│  │   ├─ glfwPollEvents                       │
│  │   ├─ app::update(window, dt, ...)        │
│  │   ├─ app::render(window, ...)            │
│  │   └─ glfwSwapBuffers                     │
│  └─ app::shutdown + glfwTerminate           │
└─────────────────────────────────────────────┘
       ↓ 使用
┌─────────────────────────────────────────────┐
│ app/<page>.cpp                               │
│   定义 app::dslAppConfig() 和                │
│        app::compose(Ui&, Screen)             │
└─────────────────────────────────────────────┘
       ↓ DSL builder API
┌─────────────────────────────────────────────┐
│ components/  +  core/                        │
│   声明式组件 + Runtime + GLES 渲染           │
└─────────────────────────────────────────────┘
```

### Android 版整体架构

```
┌─────────────────────────────────────────────────────────────┐
│ EuiActivity.kt (Kotlin 主进程)                              │
│  ├─ SurfaceView (持有渲染表面 + InputConnection)            │
│  ├─ SurfaceHolder.Callback (surface 生命周期)               │
│  ├─ Touch/Key 事件转发到 native                             │
│  ├─ IME 显示 / 隐藏 (showSoftInput / restartInput)          │
│  ├─ Config 变化通知 native (uiMode / orientation)           │
│  └─ JNI: nativeMain (启动 native 线程,只调一次)            │
└─────────────────────────────────────────────────────────────┘
       ↕ JNI
┌─────────────────────────────────────────────────────────────┐
│ glfw_shim.cpp (Android NDK 桥接层)                          │
│  ├─ ANativeWindow ↔ EGL 上下文管理                          │
│  ├─ 把 GLFW API 翻译到 ANativeWindow + EGL + Android Look   │
│  ├─ 触屏事件队列 → 模拟鼠标事件                             │
│  ├─ 字符输入队列 → 模拟 GLFW char callback                  │
│  ├─ IME 桥 (eui_ime_set_cursor_rect → JNI → Java)           │
│  └─ Surface 生命周期协调 (双层信号 + condition variable)    │
└─────────────────────────────────────────────────────────────┘
       ↕
┌─────────────────────────────────────────────────────────────┐
│ android_main.cpp                                            │
│  外层循环: 管理 GLFWwindow 生命周期 (create/destroy)        │
│  内层循环: 跑帧 (poll/update/render/swap)                   │
└─────────────────────────────────────────────────────────────┘
       ↓ 与桌面共用
┌─────────────────────────────────────────────────────────────┐
│ core/  +  components/  (原版代码,几乎不动)                   │
└─────────────────────────────────────────────────────────────┘
```

**核心思路**:在 NDK 这层做一个 GLFW 兼容 shim,让上面 `core/` / `components/` / `android_app.cpp` 的代码可以**像在桌面一样调 `glfwCreateWindow` / `glfwPollEvents` / `glfwSwapBuffers`**,不知道下面其实是 EGL + ANativeWindow + Android NDK。

---

## 🔧 详细修改清单

### 1. 新增 Kotlin Activity (`android/app/src/main/java/com/eui/neo/EuiActivity.kt`)

**职责**:

- `SurfaceView` 持有渲染表面
- `SurfaceHolder.Callback`:三个回调 (`surfaceCreated` / `surfaceChanged` / `surfaceDestroyed`) 全部转 JNI
- `onTouchEvent`:三种 action(DOWN / UP / MOVE)转 native
- `onKeyDown / onKeyUp`:物理键盘转 native
- `onPause / onResume / onDestroy`:生命周期信号
- `onConfigurationChanged`:系统主题切换实时反映到 native(`uiMode` 在 manifest 的 `configChanges` 里,所以不重建 Activity)
- **`ImeSurfaceView`(内部类)**:SurfaceView 子类,重写 `onCheckIsTextEditor()` 返回 true、`onCreateInputConnection()` 返回 `BaseInputConnection`,把 IME 的 `commitText` / `sendKeyEvent` 转发到 native
- `onNativeShowKeyboard / onNativeHideKeyboard`:被 native 回调,在 UI 线程调 `InputMethodManager.showSoftInput / hideSoftInputFromWindow`(用 `restartInput` + `SHOW_FORCED` fallback 解决回前台 IME 失活的问题)

**关键设计**:

- `threadStarted` 标志:`surfaceCreated` 首次调 `nativeMain` 启动 native 线程,后续走 `nativeSurfaceCreated` 只换 surface
- `onDestroy` 调 `nativeRequestExit + join(2000)`,确保 native 线程清退

### 2. GLFW Shim (`android/app/src/main/cpp/glfw_shim/glfw_shim.cpp`)

把所有 EUI-NEO 用到的 GLFW API 在 Android 上实现一遍。关键模块:

#### 2.1 EGL 上下文

- `glfwCreateWindow` → 用 `ANativeWindow_fromSurface` 拿 native window,`eglGetDisplay/eglInitialize/eglChooseConfig/eglCreateContext/eglCreateWindowSurface/eglMakeCurrent` 拉起 EGL
- `glfwDestroyWindow` → 反向卸下 EGL
- `glfwSwapBuffers` → `eglSwapBuffers`

#### 2.2 触屏 → 鼠标

`processAndroidEvents` 把 Android 的 ACTION_DOWN / ACTION_UP / ACTION_MOVE 翻译成 GLFW 鼠标事件:

```cpp
// 关键修复:每帧最多消费一个 DOWN/UP 状态变化
// 原因:Runtime 每帧 poll 一次 mouseButton,如果 DOWN 和 UP 同帧到达,
// 两次翻转结果不变,Runtime 看不到 pressedThisFrame → 快速点击丢失
if (!queue.empty()) {
    auto& ev = queue.front();
    if (ev.action == 1) {
        w->input.mouseButtons[GLFW_MOUSE_BUTTON_LEFT] = GLFW_PRESS;
        bridge.imeVisible.store(false);   // 用户手势 → 重置 IME 状态
    } else if (ev.action == 0) {
        w->input.mouseButtons[GLFW_MOUSE_BUTTON_LEFT] = GLFW_RELEASE;
    }
    queue.erase(queue.begin());
}
```

MOVE 事件不限速,一次性 drain 完。

#### 2.3 字符输入

`charQueue` 接收 JNI 推过来的 Unicode codepoint,在 `processAndroidEvents` 触发 `GLFWcharCallback`,Runtime 走原本的文本处理路径。

#### 2.4 Surface 生命周期

最复杂的部分。Android Surface 可能因为多种原因消失(home 键 / 屏幕关闭 / 旋转 / Activity 销毁),需要 native 端**完全销毁 GLFWwindow + EGL 上下文**,等新 surface 来再重建。Activity 重建场景下还要更新 JNI 引用。

```cpp
struct AndroidBridge {
    ANativeWindow* nativeWindow = nullptr;   // 当前绑定的 surface
    ANativeWindow* pendingWindow = nullptr;  // 等待绑定的新 surface
    std::atomic<bool> surfaceLost{false};    // 销毁请求
    std::atomic<bool> surfaceReady{false};   // 新 surface 就绪
    std::atomic<bool> exitRequested{false};  // Activity 销毁请求
    std::condition_variable surfaceCv;       // 信号
    JavaVM* jvm = nullptr;                   // JNI 回调用
    jobject activityRef = nullptr;           // ⚠️ Activity 重建时要更新
    // ...
};
```

新增的 helper:
- `eui_android_release_egl(window)` — 销毁 EGL + GLFWwindow,清状态
- `eui_android_wait_for_surface()` — 阻塞等新 surface
- `eui_android_promote_pending_window()` — 把 pending 移到 native slot
- `eui_android_surface_lost()` / `eui_android_exit_requested()` — 查询

#### 2.5 IME 桥接

`eui_ime_set_cursor_rect` 在 Android 分支(原版只有 Windows / macOS / Linux no-op)用 JNI 调 Kotlin 的 `onNativeShowKeyboard`。

```cpp
extern "C" void eui_ime_set_cursor_rect(GLFWwindow*, double, double, double, double) {
    auto& bridge = AndroidBridge::instance();
    bridge.imePingedThisFrame.store(true);
    if (!bridge.imeVisible.load()) {
        bridge.imeVisible.store(true);
        callJavaVoidMethod(bridge.showKeyboardMethod);
    }
}
```

配套的 `eui_android_poll_ime_frame()` 由主循环每帧调一次:如果当前帧没有 ping(说明焦点离开了输入框)且 IME 还在显示,就调 hide。

**核心难点**:Android 没有公共 API 通知 IME 被外部因素隐藏(比如 back 键),需要混合策略:
- 用户手势(按下)时重置 `imeVisible`
- 进入后台 / surface 销毁时重置
- Kotlin 端用 `restartInput` 让 IME 服务端丢弃缓存的 InputConnection
- `SHOW_IMPLICIT` 失败时 fallback 到 `toggleSoftInput(SHOW_FORCED, 0)`

#### 2.6 字体资源提取

Android assets 是 zip 里的,STB TrueType 需要文件系统路径。`extractFontAssets` 在 `nativeMain` 启动时把字体从 assets 抽到 `files/` 目录,通过 `eui_android_text_font_path()` / `eui_android_icon_font_path()` 返回给 app 用。

### 3. Glad GLES Stub (`android/app/src/main/cpp/glad_gles/glad/glad.h`)

桌面版用 glad 加载 GL 函数指针。Android 上 GLES 是直接链接的(`libGLESv3.so`),`<GLES3/gl3.h>` 提供所有函数,所以 glad 这层做成 stub:

```c
#include <GLES3/gl3.h>
typedef void (*GLADloadproc)(const char*);
static inline int gladLoadGLLoader(GLADloadproc) { return 1; }
```

### 4. Android Main Loop (`android/app/src/main/cpp/android_main.cpp`)

桌面版 `main.cpp` 是单层循环。Android 版改成**双层循环**:

```cpp
extern "C" int eui_android_main(void) {
    while (!eui_android_exit_requested()) {
        // 外层:管 GLFWwindow 生命周期
        if (!runWindowSession()) break;

        // 没退出但 surface 丢了 → 等新 surface
        if (!eui_android_wait_for_surface()) break;
        if (!eui_android_promote_pending_window()) continue;
    }
    app::shutdown();
    glfwTerminate();
    return 0;
}

static bool runWindowSession() {
    GLFWwindow* window = glfwCreateWindow(1080, 1920, "EUI-NEO", nullptr, nullptr);
    if (!window) return !eui_android_exit_requested();
    app::initialize(window);

    while (!eui_android_exit_requested() && !eui_android_surface_lost()) {
        // 内层:跑帧
        glfwPollEvents();
        if (app::update(window, dt, fbW, fbH, dpiScale, ptrScale, externalReady, true)) {
            needsRender = true;
        }
        eui_android_poll_ime_frame();
        if (needsRender) {
            app::render(fbW, fbH, dpiScale);
            glfwSwapBuffers(window);
        }
        // ... 帧速控制
    }

    // surface 丢了 → 干净销毁这个 session 的所有资源
    app::releaseGraphicsResources();
    core::releaseInputQueue(window);
    eui_android_release_egl(window);
    return !eui_android_exit_requested();
}
```

**为什么完全销毁 GLFWwindow**:`core/primitive.h` 和 `core/text.cpp` 都用了 `static std::unordered_map<GLFWwindow*, SharedResources>` 缓存 shader/VAO/atlas texture。Android 上 EGL 上下文销毁后,这些 GLuint 全是悬空指针。完全销毁 `GLFWwindow` 让新 window 拿到不同的指针 → 缓存 map 重新分配 → 全部 GL 资源在新 context 下重建。

### 5. Android Demo App (`android/app/src/main/cpp/android_app.cpp`)

不是简单移植 `app/gallery.cpp`(桌面 1600x1080 布局不能直接放手机 390x844 屏)。重写为:

- **侧边栏 92px** + **内容 298px** 的双栏布局
- 6 个页面(控件 / 输入 / 选择 / 图表 / 时钟 / 关于),每页独立滚动
- 每个 section 用 `cardSection` helper 包装(stack + panel 当背景 + column 当内容,zIndex 控制)
- `g_tick` + `onTimer(0.10s)` 驱动 chart 数据每 100ms 波动
- 时钟面板参考 `app/clock.cpp:composeAnalogClock`,用 12 个 rotated rect 当刻度,3 个 rotated rect 当指针
- `themeColors()` helper 统一所有控件主题
- `loadSettings()` / `saveSettings()` 把所有 g_* 状态序列化到 `<files>/settings.txt`

### 6. 上游代码改动(尽量小)

只改了 4 处,都是为了适配 Android 触屏 + IME:

#### 6.1 `core/event.h:325-329`(touch 按下时 deltaY 是假的)

```cpp
if (event.pressedThisFrame) {
    event.deltaX = 0.0;
    event.deltaY = 0.0;
}
```

**原因**:`event.deltaY = y - state.lastY`。鼠标连续移动没问题,但触屏每次按下都从任意位置开始,delta 是上次抬手到本次按下的瞬移距离(几百像素),触发 scrollView 的 onDrag 直接把滚动 offset 跳回顶部。桌面鼠标按下时光标稳定,delta 接近 0,清零没影响。

#### 6.2 `core/ime_bridge.c:140-150`

Android 分支不再 no-op,转给 `glfw_shim.cpp` 里的实现:

```c
#else
#ifndef __ANDROID__
void eui_ime_set_cursor_rect(...) { /* no-op */ }
#endif
#endif
```

#### 6.3 `core/dsl_runtime.h:18-22`  和  `core/text.cpp:33-38`

把 `RT_LOG` / `TEXT_LOG` 在 Android 上做成空宏。原版用来调试字体,每个文本元素每帧都 `__android_log_print` 一次,几十个文字组件 × 60fps = 每秒数千行日志,直接拖慢渲染线程导致点击丢失。

#### 6.4 `components/scrollview.h:62-68`

加 `.onDrag(...)` 让触屏滑动也能滚动(原版只支持鼠标滚轮):

```cpp
.onDrag([currentOffset, maxOffset, onChange](const core::dsl::DragEvent& event) {
    if (!onChange || maxOffset <= 0.0f) return;
    const float next = std::clamp(
        currentOffset - static_cast<float>(event.deltaY), 0.0f, maxOffset);
    onChange(next);
})
```

#### 6.5 `components/dropdown.h:75`

`rootHeight` 永远 = field 高度,popup 溢出 stack 外、不占布局空间:

```cpp
// 原版:open ? (field + popup) : full(预留空间)
// 移动版:不要 layout 抖动,popup 永远漂浮在下方控件之上(配合 zIndex 控制)
const float rootHeight = height_;
```

### 7. CMake 构建脚本 (`android/app/CMakeLists.txt`)

```cmake
include_directories(
    ${SHIM_ROOT}/glfw_shim     # 让 #include <GLFW/glfw3.h> 走到 shim 的实现
    ${SHIM_ROOT}/glad_gles     # 让 #include <glad/glad.h> 走到 GLES stub
    ${EUI_ROOT}                # core/ 和 components/
    ${EUI_ROOT}/3rd            # stb 等
)

target_compile_definitions(eui-neo PRIVATE __ANDROID__ GLFW_INCLUDE_NONE)

target_compile_options(eui-neo PRIVATE
    -Os -fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra
)

# 关键:用 force-include 注入 GLSL 兼容头
target_compile_options(eui-neo PRIVATE
    -include ${SHIM_ROOT}/eui_android_config.h
)

target_link_libraries(eui-neo log android EGL GLESv3)
```

`eui_android_config.h` 包含 GLES3 头 + `#define GLAD_GL_IMPLEMENTATION 0` 之类的小适配。

### 8. AndroidManifest

```xml
<activity
    android:name=".EuiActivity"
    android:configChanges="orientation|keyboardHidden|screenSize|uiMode"
    android:exported="true"
    android:launchMode="singleTask">
```

- `configChanges` 包含 `uiMode` 让系统主题切换走 `onConfigurationChanged` 而不是重建 Activity
- `singleTask` 让"从 launcher 重新打开"回到现有实例

主题 `Theme.NoTitleBar.Fullscreen` 全屏无标题栏。

---

## 🎨 开发指南

### 加一个新页面

1. 在 `android_app.cpp` 写 `pageXxx(ui, w)` 函数:

```cpp
void pageMyDemo(core::dsl::Ui& ui, float w) {
    const float cardW  = w - 2.0f * kContentPaddingX;
    const float innerW = cardW - 28.0f;

    sectionTitle(ui, "demo", "我的页面", "副标题", cardW);
    cardSection(ui, "demo", cardW, 200.0f, [&] {
        components::button(ui, "demo.btn")
            .size(innerW, 44.0f)
            .text("点我")
            .theme(themeColors())          // ← 主题感知
            .onClick([] { showToast("Hello"); })
            .build();
    });
}
```

2. 在侧边栏加菜单项(`composeSidebar`):

```cpp
sidebarItem(ui, "nav.mydemo", "演示", 0xF005, 6);  // 0xF005 是 FontAwesome star
```

3. 在 switch 加分支(`composeRightPanel`):

```cpp
case 6: pageMyDemo(ui, contentWidth); break;
```

4. 扩 `g_pageScroll[]` 数组到对应大小,改 `navOrderForPage` 的 clamp 上限和 `loadSettings` 的 clamp。

### 加一个有状态的控件

1. 全局状态:

```cpp
int g_myValue = 0;
```

2. 持久化(`saveSettings` 写,`loadSettings` 读):

```cpp
// saveSettings 里
f << "myValue=" << g_myValue << "\n";

// loadSettings 里
g_myValue = asInt("myValue", g_myValue);
```

3. compose 里读 + 写:

```cpp
components::stepper(ui, "my.step").size(innerW, 42.0f)
    .theme(themeColors())
    .value(g_myValue)
    .onChange([](long long v) {
        g_myValue = static_cast<int>(v);
        saveSettings();              // ← 任何 onChange 都要存
    })
    .build();
```

### 让自定义控件主题化

所有 `components::xxx` 系列都接 `.theme(themeColors())`:

```cpp
components::checkbox(ui, "x").theme(themeColors())
    .checked(state)
    .onChange(...)
    .build();
```

**如果忘了传 `.theme()`,组件会用 `DarkThemeColors()` 当默认,浅色模式下整片黑**。

如果用 `ui.rect`、`ui.text` 这种底层图元,要手动用 `themeColors()` 派生的 helper(`textPri()` / `surface()` / `borderC()` / `accent()` 等),不要硬编码 RGB。

### 添加自定义动画驱动

`g_tick` 模式:用 `onTimer` 驱动 needsCompose,在 compose 里用 `g_tick` 推导动画值。

```cpp
// 在 compose 末尾
ui.stack("my.tick").size(0.0f, 0.0f)
    .onTimer(0.05f, [] { ++g_tick; })   // 20Hz
    .build();

// 在 compose 中
const float t = g_tick * 0.05f;
float pulse = 0.5f + 0.3f * std::sin(t);
```

或者用 DSL 的 transition 系统让属性补间:

```cpp
ui.rect("box")
    .size(100, 100)
    .color(animating ? colorA : colorB)
    .transition(core::Transition::make(0.3f, core::Ease::OutCubic))
    .animate(core::AnimProperty::Color)
    .build();
```

后者不需要 onTimer,Runtime 会自动按时间补间。

---

## ⚠️ 已知限制

1. **Activity 重建期间(双 back 后再开)**:旧 native 线程退出和新线程启动之间有 ~50ms 窗口,期间用户输入会丢。Android Studio 里 `gradle assembleDebug` 部署偶尔会触发。
2. **IME 外部隐藏**(用户按系统的下拉手势收起键盘,不点输入框):C++ 端不知道键盘已隐藏,下次点输入框仍能正常弹出(因为我们在按下时重置 imeVisible);但如果**按 back 隐藏 IME 后什么都不点直接看页面**,C++ 端的 `imeVisible` 状态会和实际不同步几秒钟(直到下次按下或切页)。完全解决需要 `WindowInsetsCompat`(androidx)。
3. **时钟面板是只读的**:参考 `app/clock.cpp` 的设计,表盘没有 `.onClick`,只显示当前时间不能调整。
4. **下拉框 popup 高度过大时**会被 scrollView 的 `.clip()` 切到。当前 5 个选项 ~234px 在大部分手机上没问题,如果你加更多选项需要在 dropdown.h 把 popup 提到顶层 overlay 渲染(类似 dialog/toast)。
5. **横屏未深度优化**:布局会随屏幕宽度变,但侧边栏在 600px+ 宽度看起来会有点局促。如要桌面级横屏体验需重排。

---

## 📜 移植涉及的所有文件清单

| 状态 | 文件 | 说明 |
|---|---|---|
| 新增 | `android/build.gradle.kts` | 项目级 Gradle |
| 新增 | `android/settings.gradle.kts` | |
| 新增 | `android/gradle.properties` | |
| 新增 | `android/app/build.gradle.kts` | App 模块 Gradle |
| 新增 | `android/app/CMakeLists.txt` | NDK 构建 |
| 新增 | `android/app/src/main/AndroidManifest.xml` | |
| 新增 | `android/app/src/main/assets/*.ttf/.otf` | 字体资源 |
| 新增 | `android/app/src/main/java/com/eui/neo/EuiActivity.kt` | Kotlin Activity |
| 新增 | `android/app/src/main/cpp/eui_android_config.h` | NDK 配置 |
| 新增 | `android/app/src/main/cpp/android_main.cpp` | Android 主循环 |
| 新增 | `android/app/src/main/cpp/android_app.cpp` | Demo App compose |
| 新增 | `android/app/src/main/cpp/glad_gles/glad/glad.h` | Glad GLES stub |
| 新增 | `android/app/src/main/cpp/glfw_shim/glfw_shim.cpp` | GLFW → NDK shim |
| 修改 | `core/event.h` | pressedThisFrame 时清 delta |
| 修改 | `core/ime_bridge.c` | Android 分支不再 no-op |
| 修改 | `core/dsl_runtime.h` | RT_LOG 在 Android 改空宏 |
| 修改 | `core/text.cpp` | TEXT_LOG 在 Android 改空宏 |
| 修改 | `components/scrollview.h` | 加 `.onDrag` 触屏滑动 |
| 修改 | `components/dropdown.h` | rootHeight 不再随 open 变化 |

> 总共 **13 个新文件 + 6 处上游小改**。原 `core/` 和 `components/` 几乎全部直接复用。

---

## 🔍 EUI-NEO vs Dear ImGui

被问过这个问题。简单对比:

|  | Dear ImGui | EUI-NEO |
|---|---|---|
| 范式 | Immediate mode(每帧重描述) | Declarative,Runtime 内部 retained |
| 状态归属 | App 持有,库按调用顺序追踪 | App 持有,Runtime 按 `id` 缓存实例状态 |
| 动画 | 无内置 | `transition` + `animate` 是一等公民 |
| 布局 | `SameLine` + cursor advance | flexbox 风格:row/column/stack/flow + padding/margin/min/max/flex |
| 文本 | 烘焙位图字体 | STB TrueType + SDF |
| 渲染 | 输出 draw list 交给宿主 | 自带 GLES 渲染器,dirty rect 优化 |
| 输入法 | 无 | 内置 IME 桥(本 fork 加了 Android) |
| 目标场景 | 调试 / 工具 / 游戏内菜单 | 消费级 App,需要主题、动画、输入法 |

简单说:ImGui 是"写工具不在乎好不好看",EUI-NEO 是"做产品需要 native 质感"。

---

## 📄 License

继承上游 EUI-NEO 的 Apache 2.0。Android 部分代码同样使用 Apache 2.0。

字体文件遵循各自原始 License:
- JingNanJunJunTi:京南军军体,Bold-2 字重
- Font Awesome 7 Free Solid:[Font Awesome Free License](https://fontawesome.com/license/free)

## 🙏 致谢

- [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) — 原版 C++17 跨平台 UI 框架
- [GLFW](https://www.glfw.org/) — 窗口/输入抽象(虽然 Android 上是我们自己 shim 的)
- [STB](https://github.com/nothings/stb) — TrueType + SDF + image 加载
