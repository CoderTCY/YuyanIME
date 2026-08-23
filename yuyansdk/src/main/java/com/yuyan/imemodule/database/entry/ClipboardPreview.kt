package com.yuyan.imemodule.database.entry

/**
 * 剪贴板列表预览条目：content 截取前 200 字符，完整内容由 ClipboardDao.getFullContent(time) 按需加载。
 */
data class ClipboardPreview(
    val content: String,
    val isKeep: Int,
    val time: Long,
)
