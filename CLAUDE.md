# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

This is an **Android fork** of [EUI-NEO](https://github.com/sudoevolve/EUI-NEO), a C++17 declarative UI framework originally built on OpenGL + GLFW (Windows / Linux / macOS). All Android-specific code lives in `android/`; the upstream `core/` and `components/` directories are reused almost verbatim — there are exactly 6 small upstream modifications, listed below.

Two parallel build systems exist:
- **Desktop** (CMake at repo root) — produces one executable per `app/*.cpp` (`gallery`, `demo`, `calculator`, `clock`, `serial_tool`).
- **Android** (`android/` Gradle module) — produces a single `eui-neo` shared library packaged into an APK. The Android demo is `android/app/src/main/cpp/android_app.cpp`, not a port of the desktop apps.

## Build & run

### Desktop (Windows PowerShell example)
```bash
cmake -S . -B build
cmake --build build --config Release
./build/Release/gallery.exe   # one binary per app/*.cpp
```
- `-DEUI_DEPS_MODE=bundled` (default, offline) | `auto` (fetch only missing) | `fetch` (force online).
- Assets are copied next to each binary post-build.

### Android
```bash
cd android
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```
- Requires NDK 25.1+, CMake 3.22+, Android SDK API 26+ minimum.
- ABIs: `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86`.
- Or open `android/` in Android Studio Hedgehog+ → Run.

### CI
`.github/workflows/release.yml` builds desktop Windows + Linux only on tag push (`v*`). There is no Android CI.

## Architecture (the big picture)

The flow that matters across both platforms is **DSL → Runtime → GL primitives**. App code never touches OpenGL or GLFW input directly; it declares a tree each frame and lets `core::dsl::Runtime` reconcile.

```
app/*.cpp (or android_app.cpp)
   defines compose(core::dsl::Ui&, Screen)        ← declarative tree
        ↓
components/*.h                                    ← thin DSL composers
        ↓
core/dsl_runtime.h                                ← layout, hit-test, anims, dirty rect, GL sync
        ↓
core/primitive.h + core/text.cpp                  ← GL/GLES draw
        ↓
GLFW (desktop) | glfw_shim.cpp (Android: EGL + ANativeWindow)
```

Key idea on Android: the shim makes upstream code believe it's still calling GLFW. Above the shim, code is identical to desktop. Below it, the shim translates `glfwCreateWindow/PollEvents/SwapBuffers` to EGL + `ANativeWindow` + JNI.

### Desktop entry path
`main.cpp` owns the GLFW window loop and calls into `app/dsl_app.h`, which is the recommended adapter. Each `app/*.cpp` only implements:
```cpp
const DslAppConfig& app::dslAppConfig();             // chained config builder
void app::compose(core::dsl::Ui& ui, const core::dsl::Screen& screen);
```
`dsl_app.h` provides initialize/update/render/shutdown, async pump, tray, multi-window via `app::openWindow(...)`, and re-compose throttling.

### Android entry path
- `EuiActivity.kt` — Kotlin shell. Owns `SurfaceView`, forwards touch/key/IME, calls `nativeMain` once and `nativeSurfaceCreated/Destroyed` on each surface lifecycle event. `configChanges` includes `uiMode` so theme switches do not recreate the Activity.
- `android_main.cpp` — **double-loop** entry. Outer loop manages `GLFWwindow` lifetime (create/destroy on each surface). Inner loop runs frames. On surface loss it fully destroys the `GLFWwindow` and EGL context, because `core/primitive.h` and `core/text.cpp` cache GL resources in `static unordered_map<GLFWwindow*, ...>` — the pointer must change so caches re-allocate against the new context.
- `glfw_shim/glfw_shim.cpp` — implements EGL setup, touch→mouse translation (with one-DOWN-or-UP-per-frame throttling so quick taps aren't lost), Unicode char queue, surface lifecycle signaling, IME bridge, and `extractFontAssets` (STB needs filesystem paths, Android assets are inside the APK zip).
- `android_app.cpp` — Android-specific demo (sidebar + 6 pages: 控件 / 输入 / 选择 / 图表 / 时钟 / 关于). Not a port of `app/gallery.cpp` — the phone form factor required a rewrite. Persists all state to `<files>/settings.txt` via `loadSettings()` / `saveSettings()`.

### The 6 upstream modifications (small, intentional)
Touch these patterns when working in core/components to keep desktop+Android parity:
| File | Why |
|---|---|
| `core/event.h` | Zero `deltaX/Y` on `pressedThisFrame` — touch DOWN otherwise produces a huge phantom delta from last release point, which kicks scrollviews to top. |
| `core/ime_bridge.c` | `#ifndef __ANDROID__` guard so Android dispatches `eui_ime_set_cursor_rect` to the shim instead of no-op. |
| `core/dsl_runtime.h` + `core/text.cpp` | `RT_LOG` / `TEXT_LOG` become empty macros on Android — per-frame `__android_log_print` from dozens of text elements destroys frame timing. |
| `components/scrollview.h` | Adds `.onDrag(...)` so touch swiping scrolls (desktop only had wheel). |
| `components/dropdown.h` | `rootHeight` is always field height; popup floats via `zIndex` instead of expanding the layout — avoids layout jank on open. |

When extending core/components, ensure changes compile under both `__ANDROID__` (GLES3, no exceptions/RTTI) and desktop (full OpenGL + GLFW).

## Where to make changes

Prefer this order (matches `docs/SKILL.md`):

1. **App/page work** → edit or add `app/*.cpp` (desktop) or grow pages in `android/app/src/main/cpp/android_app.cpp`. Mutable page state goes in an anonymous namespace in the same file.
2. **Component work** → header-only builder under `components/*.h`. Export from `components/components.h`. Components must stay **controlled** — page passes current value, component returns next value via callback. Never store business state in a component.
3. **Core / framework** → only when the DSL/Runtime genuinely lacks a capability. Changes here affect every app target on both platforms.

### Non-negotiable conventions
- Element ids must be stable across recomposes. Internal child ids use `id + ".part"`.
- Components only compose DSL nodes; they do not own GL primitives or read raw GLFW input.
- Pass `.theme(themeColors())` to every component you place — defaulting to `DarkThemeColors()` produces black-on-black in light mode. (Android `android_app.cpp` has a `themeColors()` helper.)
- Prefer layout primitives (`row`/`column`/`stack`/`flow` + `gap`/`margin`/`align`/`padding`/`minWidth`/`flex`) over `.x()`/`.y()` math. Use absolute positioning only for overlays/decoration.
- For scrollable content, prefer `components::scrollView` (it auto-measures) over hand-maintained content-height sums.
- Frame-shape animations need explicit `.animate(core::AnimProperty::Frame)`; size/position do not animate by default.

### Adding an Android page
1. Write `pageXxx(core::dsl::Ui& ui, float w)` in `android_app.cpp`.
2. Add `sidebarItem(...)` in `composeSidebar` and a `case` in `composeRightPanel`.
3. Grow `g_pageScroll[]` and update the page-index clamp in `loadSettings` / `navOrderForPage`.
4. Any new persistent state needs lines in both `saveSettings` and `loadSettings`, and `saveSettings()` must be called from every `onChange`.

## Docs

Read selectively (Chinese filenames are the canonical docs):
- App/page flow: `docs/窗口页面.md`, `docs/DSL.md`, `docs/布局.md`, `docs/事件.md`
- Component patterns: `docs/组件.md`, `docs/DSL.md`
- Async / network: `docs/异步.md`, `docs/网络.md`
- Rendering edge cases: `docs/动画.md`, `docs/渲染流程.md`, `docs/图片.md`, `docs/基础图元文本图元.md`
- Android port internals (architecture, IME, surface lifecycle, font extraction): `android/README.md`
- Developer skill / conventions cheatsheet: `docs/SKILL.md`
