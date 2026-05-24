#include "core/ime_bridge.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <imm.h>

static LONG eui_ime_round_long(double value) {
    return (LONG)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

void eui_ime_set_cursor_rect(GLFWwindow* window, double x, double y, double width, double height) {
    if (window == 0) {
        return;
    }

    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == 0) {
        return;
    }

    HIMC context = ImmGetContext(hwnd);
    if (context == 0) {
        return;
    }

    const LONG caretX = eui_ime_round_long(x);
    const LONG caretY = eui_ime_round_long(y + height);

    COMPOSITIONFORM composition;
    composition.dwStyle = CFS_FORCE_POSITION;
    composition.ptCurrentPos.x = caretX;
    composition.ptCurrentPos.y = caretY;
    composition.rcArea.left = eui_ime_round_long(x);
    composition.rcArea.top = eui_ime_round_long(y);
    composition.rcArea.right = eui_ime_round_long(x + width);
    composition.rcArea.bottom = eui_ime_round_long(y + height);
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate;
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos.x = caretX;
    candidate.ptCurrentPos.y = caretY;
    candidate.rcArea = composition.rcArea;
    ImmSetCandidateWindow(context, &candidate);

    ImmReleaseContext(hwnd, context);
}

#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

static char euiImeRectKey;
static Class euiImeSwizzledClass = Nil;
static IMP euiImeOriginalFirstRect = nil;

static NSRect eui_ime_first_rect_for_character_range(id self, SEL selector, NSRange range, NSRangePointer actualRange) {
    if (actualRange != nil) {
        *actualRange = range;
    }

    NSValue* rectValue = objc_getAssociatedObject(self, &euiImeRectKey);
    if (rectValue != nil) {
        NSRect viewRect = [rectValue rectValue];
        NSRect windowRect = [self convertRect:viewRect toView:nil];
        return [[self window] convertRectToScreen:windowRect];
    }

    if (euiImeOriginalFirstRect != nil) {
        typedef NSRect (*FirstRectFn)(id, SEL, NSRange, NSRangePointer);
        return ((FirstRectFn)euiImeOriginalFirstRect)(self, selector, range, actualRange);
    }

    return NSMakeRect(0.0, 0.0, 0.0, 0.0);
}

static void eui_ime_prepare_view(NSView* view) {
    if (view == nil) {
        return;
    }

    Class viewClass = [view class];
    if (viewClass == euiImeSwizzledClass) {
        return;
    }

    SEL selector = @selector(firstRectForCharacterRange:actualRange:);
    Method method = class_getInstanceMethod(viewClass, selector);
    if (method == nil) {
        return;
    }

    euiImeOriginalFirstRect = method_getImplementation(method);
    method_setImplementation(method, (IMP)eui_ime_first_rect_for_character_range);
    euiImeSwizzledClass = viewClass;
}

void eui_ime_set_cursor_rect(GLFWwindow* window, double x, double y, double width, double height) {
    if (window == 0) {
        return;
    }

    NSWindow* nsWindow = glfwGetCocoaWindow(window);
    if (nsWindow == nil) {
        return;
    }

    NSView* view = [nsWindow contentView];
    if (view == nil) {
        return;
    }

    eui_ime_prepare_view(view);

    const CGFloat scale = MAX((CGFloat)1.0, [nsWindow backingScaleFactor]);
    const NSRect bounds = [view bounds];
    const CGFloat rectX = (CGFloat)x / scale;
    const CGFloat rectWidth = MAX((CGFloat)1.0, (CGFloat)width / scale);
    const CGFloat rectHeight = MAX((CGFloat)1.0, (CGFloat)height / scale);
    const CGFloat rectY = bounds.size.height - ((CGFloat)y / scale) - rectHeight;
    const NSRect viewRect = NSMakeRect(rectX, rectY, rectWidth, rectHeight);
    objc_setAssociatedObject(view, &euiImeRectKey, [NSValue valueWithRect:viewRect], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

#else

#ifndef __ANDROID__
void eui_ime_set_cursor_rect(GLFWwindow* window, double x, double y, double width, double height) {
    (void)window;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}
#endif

#endif
