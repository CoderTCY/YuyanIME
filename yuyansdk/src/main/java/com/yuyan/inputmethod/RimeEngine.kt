package com.yuyan.inputmethod

import android.view.KeyEvent
import com.yuyan.imemodule.application.CustomConstant
import com.yuyan.imemodule.application.Launcher
import com.yuyan.imemodule.manager.InputModeSwitcher
import com.yuyan.imemodule.prefs.AppPrefs
import com.yuyan.imemodule.utils.StringUtils
import com.yuyan.inputmethod.core.CandidateListItem
import com.yuyan.inputmethod.core.Rime
import com.yuyan.inputmethod.data.InputKey
import com.yuyan.inputmethod.data.KeyRecordStack
import com.yuyan.inputmethod.util.DoublePinYinUtils
import com.yuyan.inputmethod.util.LX17PinYinUtils
import com.yuyan.inputmethod.util.QwertyPinYinUtils
import com.yuyan.inputmethod.util.T9PinYinUtils

object RimeEngine {
    private val keyRecordStack = KeyRecordStack()
    private var pinyins: Array<String> = emptyArray() // 候选词界面的候选拼音列表
    var showCandidates: List<CandidateListItem> = emptyList() // 所有待展示的候选词
    var showComposition: String = "" // 候选词上方展示的拼音
    var preCommitText: String = "" // 待提交的文字
    const val MASK_CASE_LOWER = 0
    private var charCase = 0x0000
    fun init() {
        Rime.getInstance(false)
    }

    fun selectSchema(mod: String): Boolean {
        keyRecordStack.clear()
        charCase = MASK_CASE_LOWER
        Rime.startup(Launcher.instance.context, false)
        return Rime.selectSchema(mod)
    }

    fun getCurrentRimeSchema(): String {
        return Rime.getCurrentRimeSchema()
    }

    /**
     * 是否输入完毕
     */
    fun isFinish(): Boolean {
        return keyRecordStack.isEmpty()
    }

    fun onNormalKey(event: KeyEvent) {
        val keyCode = event.keyCode
        val keyChar = if(keyCode == KeyEvent.KEYCODE_APOSTROPHE) if(isFinish()) '/'.code else '\''.code
            else event.unicodeChar
        if (keyRecordStack.pushKey(event))Rime.processKey(keyChar, event.action)
        updateCandidatesOrCommitText()
    }

    fun onDeleteKey() {
        processDelAction()
        updateCandidatesOrCommitText()
    }

    fun selectCandidate(index: Int): String? {
        // 显示位置 → rime 原始候选索引的映射已下沉到 JNI（setCandidateIndexMap），此处索引恒等
        Rime.selectCandidate(index)
        keyRecordStack.pushCandidateSelectAction()
        return updateCandidatesOrCommitText()
    }

    fun getNextPageCandidates(): Array<CandidateListItem> {
        return if (Rime.hasRight()) {
            Rime.processKey(getRimeKeycodeByName("Page_Down"), 0)
            val candidates = Rime.getRimeContext()!!.candidates
            // 英文候选按输入形态筛选（筛选而非转换），并追加筛选后位置 → rime 原始索引的映射；
            // 仅英文模式筛选（中文候选的汉字是字母，会被形态规则误滤）；
            // 中文翻页无筛选，按页内索引恒等映射（与 JNI 当前页一致）
            val (filteredCandidates, indexMap) = if (InputModeSwitcher.isEnglish) {
                filterEnglishCandidates(candidates.asList(), getEchoComposition())
            } else {
                candidates.asList() to (0 until candidates.size).toList()
            }
            // 映射追加到 JNI（与 DecodingInfo 追加候选到显示列表的顺序一致）
            Rime.appendCandidateIndexMap(indexMap.ifEmpty { (0 until candidates.size).toList() }.toIntArray())
            filteredCandidates.toTypedArray()
        } else emptyArray()
    }

    fun selectPinyin(index: Int) {
        val pinyinKey = keyRecordStack.pushPinyinSelectAction(pinyins[index]) ?: return
        Rime.replaceKey(pinyinKey.posInInput, pinyinKey.t9Keys().length, pinyinKey.pinyin())
        updateCandidatesOrCommitText()
    }

    fun predictAssociationWords(text: String) {
        pinyins = emptyArray()
        if (text.isNotEmpty()) {
            val display = buildList {
                val words = Rime.getAssociateList(text)
                val firstFive = words.take(5)
                addAll(firstFive.filterNotNull().map { CandidateListItem("", it) })
                addAll(CustomEngine.predictAssociationWordsChinese(text).map { CandidateListItem("", it) })
                val remaining = words.drop(5)
                addAll(remaining.filterNotNull().map { CandidateListItem("", it) })
            }
            // 把最终显示列表整体写回 JNI 词表：选择索引与显示位置恒等，杜绝拼接错位
            Rime.setAssociateWords(display.map { it.text }.toTypedArray())
            showCandidates = display
            showComposition = ""
        }
    }

    fun selectAssociation(index: Int) {
        // JNI 词表 = 显示列表（predictAssociationWords 已写回），选择索引恒等
        Rime.chooseAssociate(index)
        // updateCandidatesOrCommitText 已消费 pending 并设置 preCommitText（联想词），
        // 此处不再覆盖（此前误用已清空的 showCandidates 取值导致联想词丢失）
        updateCandidatesOrCommitText()
    }

    fun recordExternalCommit(text: String) {
        Rime.recordExternalCommit(text)
    }


    fun reset() {
        showCandidates = emptyList()
        pinyins = emptyArray()
        showComposition = ""
        preCommitText = ""
        keyRecordStack.clear()
        Rime.clearComposition()
        if(charCase == KeyEvent.META_SHIFT_ON) charCase = MASK_CASE_LOWER
    }

    fun destroy() = Rime.destroy()

    fun processDelAction() {
        when (val lastKey = keyRecordStack.pop()) {
            is InputKey.PinyinKey -> {
                val pinyinKey = keyRecordStack.restorePinyinToT9Key(lastKey) ?: return
                replacePinyinWithT9Keys(pinyinKey)
            }
            InputKey.SelectPinyinAction -> {
                val pinyinKey = keyRecordStack.restorePinyinToT9Key() ?: return
                replacePinyinWithT9Keys(pinyinKey)
            }
            is InputKey.Apostrophe -> {
                if (!lastKey.dummy) {
                    Rime.processKey(getRimeKeycodeByName("BackSpace"), 0)
                }
            }
            else -> {
                Rime.processKey(getRimeKeycodeByName("BackSpace"), 0)
            }
        }
    }

    private fun replacePinyinWithT9Keys(pinyinKey: InputKey.PinyinKey) {
        /**
         * 当前输入状态是“你h”时，引擎默认删除行为是“ni”（删除h并且删除“你”的选中状态）
         * 可能存在引擎操作栈与记录的操作栈不一样的问题
         * 临时方案，尝试不同长度的替换，至少保证可以把拼音回退成9键
         */
        if (!Rime.replaceKey(pinyinKey.posInInput, pinyinKey.inputKeyLength, pinyinKey.t9Keys())) {
            Rime.replaceKey(pinyinKey.posInInput, pinyinKey.pinyinLength, pinyinKey.t9Keys())
        }
    }

    private fun updateCandidatesOrCommitText(): String? {
        val rimeCommit = Rime.getRimeCommit()
        if (rimeCommit != null) {
            keyRecordStack.clear()
            // rime 提交的即词条/输入原文（如 iPhone、JavaScript），不做大小写转换
            preCommitText = rimeCommit.commitText
            showComposition = ""
            showCandidates = emptyList()
            return preCommitText
        }
        val candidates = Rime.getRimeContext()?.candidates?.asList() ?: emptyList()
        val compositionText = Rime.compositionText
        // echo 回显与英文形态筛选的依据：从按键记录重建的真实输入串（含大小写），
        // 不依赖 rime preedit——引擎对 preedit 的大小写处理（折叠/保留）不可控，会导致 echo 大小写飘忽
        val echoComposition = getEchoComposition()
        showCandidates = when {
            compositionText.isNotBlank() -> {
                val phrase = CustomEngine.processPhrase(compositionText.replace("\'", ""))
                if(InputModeSwitcher.isEnglish && echoComposition.isNotEmpty() && StringUtils.isLetter(echoComposition)){
                    val firstText = candidates.firstOrNull()?.text
                    if(firstText != null && echoComposition.equals(firstText, ignoreCase = true)){
                        // rime 首候选回显了已输入串（忽略大小写）：echo 与词典候选分开处理，
                        // 回显项用逐字符输入的原文覆盖其文本，按输入原文显示且不参与形态筛选
                        candidates.first().text = echoComposition
                    } else {
                        phrase.add(0, echoComposition)
                    }
                }
                // 英文候选按输入形态筛选（筛选而非转换），并记录筛选后位置 → rime 原始索引的映射；
                // 仅英文模式筛选——中文候选的汉字是字母，会被形态规则误滤（emoji/Ext-B 反因 surrogate 保留）
                val (filteredCandidates, indexMap) = if (InputModeSwitcher.isEnglish) {
                    filterEnglishCandidates(candidates, echoComposition)
                } else {
                    candidates to emptyList()
                }
                // 显示位置 → rime 索引映射下沉到 JNI：📋 前缀（含英文 echo）占位 -1，其余映射引擎索引；
                // 选择时索引恒等，映射与显示列表由同一处构建，杜绝偏移漂移
                Rime.setCandidateIndexMap(
                    buildList {
                        repeat(phrase.size) { add(-1) }
                        addAll(indexMap.ifEmpty { candidates.indices.toList() })
                    }.toIntArray()
                )
                phrase.map { content -> CandidateListItem("📋", content) }.toMutableList().plus(filteredCandidates)
            }
            else -> {
                // 无自定义前缀/无筛选：空映射即恒等兜底
                Rime.setCandidateIndexMap(IntArray(0))
                candidates
            }
        }
        var count = Rime.compositionText.count { it in 'A'..'Z' }
        if (count > 0) {
            keyRecordStack.forEachReversed { inputKey ->
                if (inputKey is InputKey.T9Key) inputKey.consumed = count-- <= 0
            }
        }
        val composition = getCurrentComposition(candidates)
        val rimeSchema = Rime.getCurrentRimeSchema()
        pinyins = when (rimeSchema) {
            CustomConstant.SCHEMA_ZH_T9 -> {
                T9PinYinUtils.t9KeyToPinyin(compositionText.split('\'').firstOrNull { part -> part.isNotEmpty() && part.all { it.isUpperCase() } } ?: "")
            }
            CustomConstant.SCHEMA_ZH_DOUBLE_LX17 -> {
                LX17PinYinUtils.lx17KeyToPinyin(compositionText.split('\'').firstOrNull { part -> part.isNotEmpty() && part.all { it.isUpperCase() } } ?: "")
            }
            else -> {
                emptyArray()
            }
        }
        showComposition = composition
        preCommitText = ""
        return null
    }

    /**
     * 从按键记录重建真实输入串（含大小写）：T9Key 存大写、QwertKey 存小写，
     * 逐字符还原用户实际键入的形态，作为 echo 回显与英文形态筛选的依据。
     */
    private fun getEchoComposition(): String {
        val sb = StringBuilder()
        keyRecordStack.forEach { key ->
            when (key) {
                is InputKey.T9Key, is InputKey.QwertKey -> sb.append(key.toString())
                is InputKey.Apostrophe -> sb.append('\'')
                else -> {}
            }
        }
        return sb.toString()
    }

    /**
     * 英文候选大小写形态筛选（筛选而非转换）：
     * rime 英文词典以小写编码匹配，同一单词的各种大小写形态变体（java/Java/JAVA/JAva…）都会进入候选，
     * 这里按已输入串的实际大小写形态过滤，只保留形态匹配的词条：
     * - 大写锁定时只保留全大写词条（MA → MARK）；
     * - 输入全小写 → 只保留全小写词条（javasc → javascript）；
     * - 输入首字母大写 → 只保留 Title 词条（Jav → Java/JavaScript）；
     * - 输入全大写（非锁定，如连按 ⇧）→ 只保留 Title 词条（Mark）；
     * - 混合输入 → 逐字符前缀大小写一致（iPh → iPhone、JavaSc → JavaScript）。
     * echo 回显项（与已输入串相同）、📋 自定义项、含非字母的词条（中文等）始终保留；
     * 筛选结果为空时降级返回原列表。
     * @return 筛选后的候选列表 + 筛选后位置 → 原列表位置的索引映射
     */
    private fun filterEnglishCandidates(items: List<CandidateListItem>, input: String): Pair<List<CandidateListItem>, List<Int>> {
        if (input.isEmpty() || !StringUtils.isLetter(input)) return items to emptyList()
        val capsLock = charCase == KeyEvent.META_CAPS_LOCK_ON
        val shapeMatch: (String) -> Boolean = { word ->
            when {
                capsLock -> word.all { !it.isLetter() || it.isUpperCase() }
                input.all { it.isLowerCase() } -> word.all { !it.isLetter() || it.isLowerCase() }
                input.all { it.isUpperCase() } ->
                    word.first().isUpperCase() && word.drop(1).all { !it.isLetter() || it.isLowerCase() }
                input.first().isUpperCase() && input.drop(1).all { it.isLowerCase() } ->
                    word.first().isUpperCase() && word.drop(1).all { !it.isLetter() || it.isLowerCase() }
                else -> word.length >= input.length && word.zip(input).all { (c, i) -> !c.isLetter() || c.isUpperCase() == i.isUpperCase() }
            }
        }
        val kept = items.mapIndexedNotNull { index, item ->
            if (item.comment == "📋" || item.text.equals(input, ignoreCase = true) ||
                !item.text.all { it.isLetter() } || shapeMatch(item.text)
            ) index to item else null
        }
        return if (kept.isEmpty()) items to emptyList()
        else kept.map { it.second } to kept.map { it.first }
    }

    /**
     * 拿到候选词拼音组合
     */
    fun getPrefixs(): Array<String> {
        return pinyins
    }

    private fun getCurrentComposition(candidates: List<CandidateListItem>): String {
        val composition = Rime.compositionText
        val rimeSchema = Rime.getCurrentRimeSchema()
        if(rimeSchema == CustomConstant.SCHEMA_EN) return ""
        if(composition.isEmpty()) return ""
        if(candidates.isEmpty()) return composition
        val comment = candidates.first().comment
        val result = when {
            comment.isNotBlank() && comment.startsWith("~") -> composition
            rimeSchema == CustomConstant.SCHEMA_ZH_T9 -> {
                T9PinYinUtils.getT9Composition(composition, comment)
            }
            rimeSchema.startsWith(CustomConstant.SCHEMA_ZH_DOUBLE_FLYPY) -> {
                if(!AppPrefs.getInstance().keyboardSetting.keyboardDoubleInputKey.getValue()) composition
                else DoublePinYinUtils.getDoublePinYinComposition(rimeSchema, composition, comment)
            }
            else -> {
                QwertyPinYinUtils.getQwertyComposition(composition, comment)
            }
        }
        // 九键/乱序17 输入层固定大写，派生拼音显示统一转大写，与 Shift/Caps 状态无关
        val display = if (rimeSchema == CustomConstant.SCHEMA_ZH_T9 || rimeSchema == CustomConstant.SCHEMA_ZH_DOUBLE_LX17) {
            result.uppercase()
        } else result
        return if (!composition.endsWith("'") && display.endsWith("'")) display.dropLast(1) else display
    }

    /**
     * 设置输入法搜索参数
     */
    fun setImeOption(option: String, value: Boolean) {
        Rime.setOption(option, value)
    }

    /**
     * 获取Rime定义键值
     */
    private fun getRimeKeycodeByName(name: String) : Int {
        return Rime.getRimeKeycodeByName(name)
    }

    fun setCharCase(charCase: Int) {
        this.charCase = charCase
    }

}