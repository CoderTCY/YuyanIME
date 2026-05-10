package com.yuyan.imemodule.candidate

import android.annotation.SuppressLint
import android.content.Context
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.inputmethod.EditorInfo
import android.widget.RelativeLayout
import android.widget.Toast
import androidx.core.text.isDigitsOnly
import androidx.core.view.postDelayed
import com.yuyan.imemodule.R
import com.yuyan.imemodule.callback.CandidateViewListener
import com.yuyan.imemodule.data.emojicon.EmojiconData.SymbolPreset
import com.yuyan.imemodule.data.theme.ThemeManager
import com.yuyan.imemodule.database.DataBaseKT
import com.yuyan.imemodule.database.entry.Phrase
import com.yuyan.imemodule.keyboard.KeyboardManager
import com.yuyan.imemodule.keyboard.container.ClipBoardContainer
import com.yuyan.imemodule.keyboard.container.T9TextContainer
import com.yuyan.imemodule.manager.InputModeSwitcherManager
import com.yuyan.imemodule.prefs.AppPrefs.Companion.getInstance
import com.yuyan.imemodule.prefs.behavior.SkbMenuMode
import com.yuyan.imemodule.service.DecodingInfo
import com.yuyan.imemodule.service.ImeService
import com.yuyan.imemodule.singleton.EnvironmentSingleton
import com.yuyan.imemodule.utils.DevicesUtils
import com.yuyan.imemodule.utils.StringUtils
import com.yuyan.imemodule.view.widget.LifecycleRelativeLayout

/**
 * 输入法主界面。
 * 包含拼音显示、候选词栏、键盘界面等。
 */
@SuppressLint("ViewConstructor")
class CandidateView(context: Context, private val service: ImeService) : LifecycleRelativeLayout(context) {
    private val appPrefs = getInstance()
    private var chinesePrediction = true
//    var isAddPhrases = false
    private var mImeState = ImeState.STATE_IDLE
    private val mChoiceNotifier = ChoiceNotifier()
    var mSkbRoot: RelativeLayout
    var mSkbCandidatesBarView: FloatCandidateBar

    init {
        InputModeSwitcherManager.reset()
        mSkbRoot = LayoutInflater.from(context).inflate(R.layout.sdk_candidate_container, this, false) as RelativeLayout
        addView(mSkbRoot)
        mSkbCandidatesBarView = mSkbRoot.findViewById(R.id.candidates_bar)
        DecodingInfo.candidatesLiveData.observe(this) {
            updateCandidateBar()
        }
        initView(context)
    }

    @SuppressLint("ClickableViewAccessibility")
    fun initView(context: Context) {
        mSkbCandidatesBarView.initialize(mChoiceNotifier)
        val env = EnvironmentSingleton.instance
        mSkbRoot.layoutParams.width = env.inputAreaWidth
        updateTheme()
    }

    fun updateTheme() {
        setBackgroundResource(android.R.color.transparent)
        val activeTheme = ThemeManager.activeTheme
        val keyTextColor = activeTheme.keyTextColor
        mSkbCandidatesBarView.updateTheme(keyTextColor)
    }

    fun processKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if(InputModeSwitcherManager.isEnglish) return false
        // 字母、数字、符号、空格
        if (keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z) return true
        if (keyCode >= KeyEvent.KEYCODE_0 && keyCode <= KeyEvent.KEYCODE_9) return true
        if (keyCode == KeyEvent.KEYCODE_SPACE) return true
        if (keyCode == KeyEvent.KEYCODE_APOSTROPHE) return true
        // 编辑键
        if (keyCode == KeyEvent.KEYCODE_DEL) return true
        if (keyCode == KeyEvent.KEYCODE_ENTER) return true
        if (keyCode == KeyEvent.KEYCODE_TAB) return true
        // 方向键
        if (keyCode >= KeyEvent.KEYCODE_DPAD_UP && keyCode <= KeyEvent.KEYCODE_DPAD_RIGHT) return true
        return false
    }

    fun processKeyUp(event: KeyEvent): Boolean {
        InputModeSwitcherManager.resetCharCase()
        if (processFunctionKeys(event)) return true
        return when {
            InputModeSwitcherManager.isChinese -> processInput(event)
            else -> false
        }
    }

    private fun processFunctionKeys(event: KeyEvent): Boolean {
        return when (val keyCode = event.keyCode) {
            KeyEvent.KEYCODE_BACK -> if (service.isInputViewShown) { requestHideSelf(); true } else false
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_SPACE -> {
                if (DecodingInfo.isFinish || (DecodingInfo.isAssociate && !mSkbCandidatesBarView.isActiveCand())) {
                    sendKeyEvent(keyCode)
                    resetToIdleState()
                } else {
                    chooseAndUpdate()
                }
                if(keyCode == KeyEvent.KEYCODE_SPACE && event.isCtrlPressed ){
                    InputModeSwitcherManager.switchModeForUserKey(InputModeSwitcherManager.USER_DEF_KEYCODE_LANG_2)
                    resetToIdleState()
                    Toast.makeText(context, if(InputModeSwitcherManager.isEnglish)"语燕输入法-英文" else "语燕输入法-拼音", Toast.LENGTH_LONG).show()
                }
                true
            }
            KeyEvent.KEYCODE_CLEAR -> {
                resetToIdleState()
                true
            }
            KeyEvent.KEYCODE_ENTER -> {
                if (DecodingInfo.isFinish || DecodingInfo.isAssociate) sendKeyEvent(keyCode)
                else commitDecInfoText(DecodingInfo.composingStrForCommit)
                resetToIdleState()
                true
            }
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT -> {
                if (event.flags != KeyEvent.FLAG_SOFT_KEYBOARD && !DecodingInfo.isCandidatesListEmpty) {
                    mSkbCandidatesBarView.updateActiveCandidateNo(keyCode)
                } else if (DecodingInfo.isFinish || DecodingInfo.isAssociate) {
                    sendKeyEvent(keyCode)
                } else {
                    chooseAndUpdate()
                }
                true
            }
            else -> false
        }
    }

    private fun processInput(event: KeyEvent): Boolean {
        val keyCode = event.keyCode
        val keyChar = event.unicodeChar
        val label = keyChar.toChar().toString()
        return when {
            keyCode == KeyEvent.KEYCODE_DEL -> {
                if (DecodingInfo.isFinish || DecodingInfo.isAssociate) {
                    sendKeyEvent(keyCode)
                } else {
                    DecodingInfo.deleteAction()
                    updateCandidate()
                }
                true
            }
            label.isDigitsOnly() ->{
                chooseAndUpdate(label.toInt() - 1)
                true
            }
            (Character.isLetterOrDigit(keyChar) && keyCode != KeyEvent.KEYCODE_0) || keyCode == KeyEvent.KEYCODE_APOSTROPHE || keyCode == KeyEvent.KEYCODE_SEMICOLON -> {
                DecodingInfo.inputAction(event)
                updateCandidate()
                true
            }
            keyCode != 0 -> {
                if (!DecodingInfo.isCandidatesListEmpty && !DecodingInfo.isAssociate) chooseAndUpdate()
                sendKeyEvent(keyCode)
                resetToIdleState()
                true
            }
            label.isNotEmpty() -> {
                if (!DecodingInfo.isCandidatesListEmpty && !DecodingInfo.isAssociate) chooseAndUpdate()
                if (SymbolPreset.containsKey(label)) commitPairSymbol(label) else commitText(label)
                true
            }
            else -> false
        }
    }

    fun resetToIdleState() {
        if (mImeState == ImeState.STATE_IDLE)return
        resetCandidateWindow()
        mImeState = ImeState.STATE_IDLE
    }

    fun chooseAndUpdate(candId: Int = mSkbCandidatesBarView.getActiveCandNo()) {
        val choice = DecodingInfo.chooseDecodingCandidate(candId)
        if (DecodingInfo.isEngineFinish || DecodingInfo.isAssociate) {
            commitDecInfoText(choice)
            resetToIdleState()
        }
    }

    private fun updateCandidate() {
        DecodingInfo.updateDecodingCandidate()
        if (!DecodingInfo.isFinish) {
            mImeState = if(DecodingInfo.isEngineFinish)ImeState.STATE_PREDICT else ImeState.STATE_INPUT
        } else {
            resetToIdleState()
        }
    }

    fun updateCandidateBar() = mSkbCandidatesBarView.showCandidates()

    private fun resetCandidateWindow() {
        DecodingInfo.reset()
        (KeyboardManager.instance.currentContainer as? T9TextContainer)?.updateSymbolListView()
    }

    inner class ChoiceNotifier internal constructor() : CandidateViewListener {
        override fun onClickChoice(choiceId: Int) {
            DevicesUtils.tryPlayKeyDown()
            DevicesUtils.tryVibrate(KeyboardManager.instance.currentContainer)
            chooseAndUpdate(choiceId)
        }

        override fun onClickMore(level: Int) {
            if (level == 0) {
                onSettingsMenuClick(SkbMenuMode.CandidatesMore)
            } else {
                KeyboardManager.instance.switchKeyboard()
                (KeyboardManager.instance.currentContainer as? T9TextContainer)?.updateSymbolListView()
            }
        }

        override fun onClickMenu(skbMenuMode: SkbMenuMode) = onSettingsMenuClick(skbMenuMode)

        override fun onClickClearCandidate() {
            resetToIdleState()
            KeyboardManager.instance.switchKeyboard()
        }

        override fun onClickClearClipBoard() {
            DataBaseKT.instance.clipboardDao().deleteAllExceptKeep()
            (KeyboardManager.instance.currentContainer as? ClipBoardContainer)?.showClipBoardView(SkbMenuMode.ClipBoard)
        }
    }

    fun onSettingsMenuClick(skbMenuMode: SkbMenuMode, extra: Phrase? = null) {

    }

    enum class ImeState { STATE_IDLE, STATE_INPUT, STATE_PREDICT }


    fun requestHideSelf() = service.requestHideSelf(0)

    private fun sendKeyEvent(keyCode: Int) {
        when (keyCode) {
            KeyEvent.KEYCODE_ENTER -> service.sendEnterKeyEvent()
            in KeyEvent.KEYCODE_DPAD_UP..KeyEvent.KEYCODE_DPAD_RIGHT -> {
                service.sendCombinationKeyEvents(keyCode)
            }
            else -> service.sendCombinationKeyEvents(keyCode)
        }
    }

    private fun setComposingText(text: CharSequence) {
        service.setComposingText(text)
    }

    private fun commitText(text: String) {
        service.commitText(StringUtils.converted2FlowerTypeface(text))
    }

    private fun commitPairSymbol(text: String) {
        if (appPrefs.input.symbolPairInput.getValue()) {
            service.commitText(text + SymbolPreset[text]!!)
            postDelayed(300) { service.sendCombinationKeyEvents(KeyEvent.KEYCODE_DPAD_LEFT) }
        } else {
            service.commitText(text)
        }
    }

    private fun commitDecInfoText(resultText: String?) {
        resultText ?: return
        service.commitText(StringUtils.converted2FlowerTypeface(resultText))
        if (InputModeSwitcherManager.isEnglish){
            service.finishComposingText()
            if(appPrefs.input.abcSpaceAuto.getValue()) service.commitText(" ")
            resetToIdleState()
        }
    }

    fun onStartInput(editorInfo: EditorInfo?, restarting: Boolean) {
        if(editorInfo != null)InputModeSwitcherManager.requestInputWithSkb(editorInfo)
        if (!restarting) {
            resetToIdleState()
        }
    }

    fun onWindowShown() {
        chinesePrediction = appPrefs.input.chinesePrediction.getValue()
    }

    fun onWindowHidden() {
        KeyboardManager.instance.switchKeyboard()
        resetToIdleState()
    }
}