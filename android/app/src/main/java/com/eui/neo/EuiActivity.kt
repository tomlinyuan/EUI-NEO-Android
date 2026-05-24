package com.eui.neo

import android.app.Activity
import android.content.Context
import android.content.res.Configuration
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.content.res.AssetManager

class EuiActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var surfaceView: ImeSurfaceView
    private var threadStarted = false
    private var mainThread: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        surfaceView = ImeSurfaceView(this)
        surfaceView.holder.addCallback(this)
        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true
        setContentView(surfaceView)
    }

    private fun isSystemDark(): Boolean {
        val mode = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK
        return mode == Configuration.UI_MODE_NIGHT_YES
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        // System theme can change while the app is running (Settings → Display
        // → Dark theme). Push the new value to native so "auto" mode reacts.
        nativeSetDarkMode(isSystemDark())
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (!threadStarted) {
            threadStarted = true
            mainThread = Thread {
                nativeMain(holder.surface, assets)
            }.also { it.start() }
            // Push initial system theme right after native is bootstrapped.
            // post() ensures nativeMain has had a chance to register the
            // JNI hook before we call nativeSetDarkMode.
            surfaceView.post { nativeSetDarkMode(isSystemDark()) }
        } else {
            nativeSurfaceCreated(holder.surface)
            surfaceView.post { nativeSetDarkMode(isSystemDark()) }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        nativeResize(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        nativeSurfaceDestroyed()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val actionIndex = event.actionIndex
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> 1
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> 0
            MotionEvent.ACTION_MOVE -> 2
            else -> return super.onTouchEvent(event)
        }
        nativeTouchEvent(
            event.getX(actionIndex),
            event.getY(actionIndex),
            action
        )
        return true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        nativeKeyEvent(keyCode, 1)
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        nativeKeyEvent(keyCode, 0)
        return super.onKeyUp(keyCode, event)
    }

    override fun onPause() {
        super.onPause()
        nativePause()
        // System dismisses the keyboard when we lose focus; drop our side of
        // the InputConnection so the IME doesn't hold a stale reference.
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(surfaceView.windowToken, 0)
    }

    override fun onResume() {
        super.onResume()
        nativeResume()
        // Pre-focus the view so the IME has a valid focus target the moment
        // an input field gets re-focused on the native side.
        surfaceView.post { surfaceView.requestFocus() }
    }

    override fun onDestroy() {
        nativeRequestExit()
        mainThread?.join(2000)
        mainThread = null
        threadStarted = false
        super.onDestroy()
    }

    // Called from native (any thread) — must marshal to UI thread.
    @Suppress("unused")
    fun onNativeShowKeyboard() {
        runOnUiThread {
            // Defer through post() so this runs after any pending focus / view
            // tree changes settle (important after returning from background).
            surfaceView.post {
                if (!surfaceView.isFocused) {
                    surfaceView.requestFocus()
                }
                val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                // restartInput tells the IME to drop its cached InputConnection
                // and ask onCreateInputConnection again — essential after the
                // activity comes back from background, where the IME service
                // may still hold a stale reference to the previous connection.
                imm.restartInput(surfaceView)
                // SHOW_IMPLICIT can return false right after onResume (Android
                // decides there was no user gesture justifying the show). Fall
                // back to a forced toggle which doesn't depend on view state.
                if (!imm.showSoftInput(surfaceView, InputMethodManager.SHOW_IMPLICIT)) {
                    imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0)
                }
            }
        }
    }

    @Suppress("unused")
    fun onNativeHideKeyboard() {
        runOnUiThread {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.hideSoftInputFromWindow(surfaceView.windowToken, 0)
        }
    }

    // SurfaceView subclass that participates in the IME pipeline. Its
    // InputConnection forwards committed text + key events to native.
    private inner class ImeSurfaceView(context: Context) : SurfaceView(context) {
        init {
            isFocusable = true
            isFocusableInTouchMode = true
        }

        override fun onCheckIsTextEditor(): Boolean = true

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
            outAttrs.inputType = EditorInfo.TYPE_CLASS_TEXT or EditorInfo.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_FLAG_NO_EXTRACT_UI
            return object : BaseInputConnection(this, false) {
                override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                    if (text == null) return true
                    var i = 0
                    while (i < text.length) {
                        val ch = text[i]
                        val codepoint: Int = if (Character.isHighSurrogate(ch) && i + 1 < text.length) {
                            val low = text[i + 1]
                            i += 2
                            Character.toCodePoint(ch, low)
                        } else {
                            i += 1
                            ch.code
                        }
                        nativeTextInput(codepoint)
                    }
                    return true
                }

                override fun sendKeyEvent(event: KeyEvent): Boolean {
                    if (event.action == KeyEvent.ACTION_DOWN) {
                        when (event.keyCode) {
                            KeyEvent.KEYCODE_DEL,
                            KeyEvent.KEYCODE_FORWARD_DEL,
                            KeyEvent.KEYCODE_ENTER,
                            KeyEvent.KEYCODE_DPAD_LEFT,
                            KeyEvent.KEYCODE_DPAD_RIGHT,
                            KeyEvent.KEYCODE_DPAD_UP,
                            KeyEvent.KEYCODE_DPAD_DOWN -> nativeKeyEvent(event.keyCode, 1)
                            else -> {
                                val unicode = event.unicodeChar
                                if (unicode > 0) nativeTextInput(unicode)
                            }
                        }
                    } else if (event.action == KeyEvent.ACTION_UP) {
                        when (event.keyCode) {
                            KeyEvent.KEYCODE_DEL,
                            KeyEvent.KEYCODE_FORWARD_DEL,
                            KeyEvent.KEYCODE_ENTER,
                            KeyEvent.KEYCODE_DPAD_LEFT,
                            KeyEvent.KEYCODE_DPAD_RIGHT,
                            KeyEvent.KEYCODE_DPAD_UP,
                            KeyEvent.KEYCODE_DPAD_DOWN -> nativeKeyEvent(event.keyCode, 0)
                        }
                    }
                    return true
                }

                override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                    repeat(beforeLength) { nativeKeyEvent(KeyEvent.KEYCODE_DEL, 1) }
                    return true
                }
            }
        }
    }

    private external fun nativeMain(surface: Surface, assetManager: AssetManager)
    private external fun nativeResize(width: Int, height: Int)
    private external fun nativeTouchEvent(x: Float, y: Float, action: Int)
    private external fun nativeKeyEvent(keyCode: Int, action: Int)
    private external fun nativeTextInput(codepoint: Int)
    private external fun nativeRequestExit()
    private external fun nativeSurfaceCreated(surface: Surface)
    private external fun nativeSurfaceDestroyed()
    private external fun nativePause()
    private external fun nativeResume()
    private external fun nativeSetDarkMode(dark: Boolean)

    companion object {
        init {
            System.loadLibrary("eui-neo")
        }
    }
}
