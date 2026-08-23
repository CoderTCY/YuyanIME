package com.yuyan.imemodule.service

import android.content.ClipboardManager.OnPrimaryClipChangedListener
import com.yuyan.imemodule.application.Launcher
import com.yuyan.imemodule.database.DataBaseKT
import com.yuyan.imemodule.database.entry.Clipboard
import com.yuyan.imemodule.prefs.AppPrefs
import com.yuyan.imemodule.utils.clipboardManager
import kotlin.math.max

/**
 * 剪切板监听
 * 移除使用广播监听方式，解决部分手机后台无法启动监听服务异常(API level 31)。
 */
object ClipboardHelper : OnPrimaryClipChangedListener {
    /** 单条剪贴板内容最大字符数（1MB，按 UTF-16 每字符 2 字节计；高于 Binder IPC ~50 万字符的实际天花板） */
    const val MAX_CLIP_LENGTH = 1024 * 1024 / 2

    fun init() {
        Launcher.instance.context.clipboardManager.addPrimaryClipChangedListener(this)
    }

    override fun onPrimaryClipChanged() {
        val isClipboardListening = AppPrefs.getInstance().clipboard.clipboardListening.getValue()
        if(isClipboardListening) {
           val item = Launcher.instance.context.clipboardManager.primaryClip?.getItemAt(0)
            item?.takeIf { it.text?.isNotBlank() == true }?.let {
                    // 单条上限 4MB：SQLite 可存更大，但超长文本会拖慢列表查询/上屏
                    val data = if(it.text.length > MAX_CLIP_LENGTH) it.text.substring(0, MAX_CLIP_LENGTH) else it.text.toString()
                    DataBaseKT.instance.clipboardDao().insert(Clipboard(content = data))
                    val num = max(DataBaseKT.instance.clipboardDao().getCount() - AppPrefs.getInstance().clipboard.clipboardHistoryLimit.getValue(), 0)
                    DataBaseKT.instance.clipboardDao().deleteOldest(num)
                    if (AppPrefs.getInstance().clipboard.clipboardSuggestion.getValue()) {
                        // 仅存时间信号，内容经 DB 查询，避免大文本写入 SharedPreferences
                        AppPrefs.getInstance().internal.clipboardUpdateTime.setValue(System.currentTimeMillis())
                    }
                }
        }
    }
}
