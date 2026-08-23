package com.yuyan.imemodule.database.dao

import androidx.room.Dao
import androidx.room.Query
import com.yuyan.imemodule.database.BaseDao
import com.yuyan.imemodule.database.entry.Clipboard
import com.yuyan.imemodule.database.entry.ClipboardPreview
@Dao
interface ClipboardDao : BaseDao<Clipboard> {

    /** 列表展示用：仅取内容预览，避免大文本整条载入 */
    @Query("select content, isKeep, time, substr(content, 1, 200) as preview from clipboard ORDER BY isKeep DESC, time DESC")
    fun getAllPreview(): List<ClipboardPreview>

    /** 最新一条的完整内容（剪贴板提示用） */
    @Query("select content from clipboard where isKeep = 0 order by time desc limit 1")
    fun getLatestContent(): String?

    @Query("select content from clipboard where time = :time limit 1")
    fun getFullContent(time: Long): String?


    @Query("delete from clipboard where time = :time")
    fun deleteByTime(time: Long)

    @Query("delete from clipboard")
    fun deleteAll()

    @Query("SELECT COUNT(*) FROM clipboard")
    fun getCount(): Int

    @Query("DELETE FROM clipboard WHERE content IN ( SELECT content FROM clipboard ORDER BY time ASC LIMIT :overflow)")
    fun deleteOldest(overflow: Int)

    @Query("DELETE FROM clipboard WHERE isKeep = 0")
    fun deleteAllExceptKeep()
}
