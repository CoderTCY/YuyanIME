package com.yuyan.imemodule.keyboard

import com.yuyan.imemodule.application.Launcher
import com.yuyan.imemodule.manager.InputModeSwitcher
import com.yuyan.imemodule.keyboard.container.BaseContainer
import com.yuyan.imemodule.keyboard.container.CandidatesContainer
import com.yuyan.imemodule.keyboard.container.ClipBoardContainer
import com.yuyan.imemodule.keyboard.container.HandwritingContainer
import com.yuyan.imemodule.keyboard.container.InputBaseContainer
import com.yuyan.imemodule.keyboard.container.InputViewParent
import com.yuyan.imemodule.keyboard.container.QwertyContainer
import com.yuyan.imemodule.keyboard.container.SettingsContainer
import com.yuyan.imemodule.keyboard.container.SymbolContainer
import com.yuyan.imemodule.keyboard.container.T9TextContainer
import com.yuyan.imemodule.prefs.AppPrefs

/**
 * 键盘显示管理类
 */
class KeyboardManager {
    enum class KeyboardType {
        T9, QWERTY, QWERTYABC, NUMBER, SYMBOL, SETTINGS, HANDWRITING, CANDIDATES, ClipBoard, TEXTEDIT
    }
    private lateinit var mInputView: InputView
    private lateinit var mKeyboardRootView: InputViewParent
    private val keyboards = HashMap<KeyboardType, BaseContainer?>()
    private lateinit var mCurrentKeyboardName: KeyboardType
    var currentContainer: BaseContainer? = null
        private set

    fun setData(keyboardRootView: InputViewParent, inputView: InputView) {
        keyboards.clear() // TODO 清空缓存界面，发现调用 PinyinService.onCreateInputView时，原输入界面全部会失效。
        mKeyboardRootView = keyboardRootView
        mInputView = inputView
    }

    fun clearKeyboard() {
        keyboards.clear()
        if (::mInputView.isInitialized) mInputView.initView(mInputView.context)
    }

    fun switchKeyboard(layout: Int = InputModeSwitcher.skbLayout) {
        val keyboardName = when (layout) {
            0x1000 -> KeyboardType.QWERTY
            0x4000 -> KeyboardType.QWERTYABC
            0x3000 -> KeyboardType.HANDWRITING
             0x5000 -> KeyboardType.NUMBER
             0x8000 -> KeyboardType.TEXTEDIT
            else -> KeyboardType.T9
        }
        switchKeyboard(keyboardName)
        if (::mInputView.isInitialized)mInputView.updateCandidateBar()
    }

    fun switchKeyboard(keyboardName: KeyboardType) {
        if (!::mKeyboardRootView.isInitialized) return
        var container = keyboards[keyboardName]
        if (container == null) {
            container = when (keyboardName) {
                KeyboardType.CANDIDATES ->  CandidatesContainer(Launcher.instance.context, mInputView)
                KeyboardType.HANDWRITING -> HandwritingContainer(Launcher.instance.context, mInputView)
                KeyboardType.NUMBER -> T9TextContainer(Launcher.instance.context, mInputView, InputModeSwitcher.MASK_SKB_LAYOUT_NUMBER)
                KeyboardType.QWERTY -> QwertyContainer(Launcher.instance.context, mInputView, InputModeSwitcher.MASK_SKB_LAYOUT_QWERTY_PINYIN)
                KeyboardType.SETTINGS -> SettingsContainer(Launcher.instance.context, mInputView)
                KeyboardType.SYMBOL -> SymbolContainer(Launcher.instance.context, mInputView)
                 KeyboardType.QWERTYABC -> QwertyContainer(Launcher.instance.context, mInputView, InputModeSwitcher.MASK_SKB_LAYOUT_QWERTY_ABC)
                 KeyboardType.ClipBoard -> ClipBoardContainer(Launcher.instance.context, mInputView)
                KeyboardType.TEXTEDIT -> QwertyContainer(Launcher.instance.context, mInputView, InputModeSwitcher.MASK_SKB_LAYOUT_TEXTEDIT)
                else ->  T9TextContainer(Launcher.instance.context, mInputView, AppPrefs.getInstance().internal.inputDefaultMode.getValue() and InputModeSwitcher.MASK_SKB_LAYOUT)
            }
            container.updateSkbLayout()
            keyboards[keyboardName] = container
        }
        mKeyboardRootView.showView(container)
        mCurrentKeyboardName = keyboardName
        currentContainer = container
        // 符号键盘时候选栏需刷新为菜单栏模式（只显示顶部菜单栏，不显示候选词）
        if (::mInputView.isInitialized && keyboardName == KeyboardType.SYMBOL) mInputView.updateCandidateBar()
    }

    val isInputKeyboard: Boolean
        get() = currentContainer is InputBaseContainer

    companion object {
        private var mInstance: KeyboardManager? = null
        @JvmStatic
        val instance: KeyboardManager
            get() {
                if (null == mInstance) {
                    mInstance = KeyboardManager()
                }
                return mInstance!!
            }
    }
}