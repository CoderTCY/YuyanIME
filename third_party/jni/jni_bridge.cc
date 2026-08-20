// jni_bridge.cc
// JNI 桥：保持 com.yuyan.inputmethod.core.Rime 的 16 个 native 方法签名，
// 引擎主体为官方 librime（静态库 rime-static 链接进 libyuyanime.so）。
//
// 与旧 yuyan 定制引擎的差异适配：
//  1. 主流程全部走 librime 官方 C API（rime_get_api）
//  2. replaceRimeKey 用 librime 内部 API（rime::Service/Session/Context）——
//     官方 C API 无局部替换；set_input 不刷新候选，需补 RefreshNonConfirmedComposition
//  3. 按键归一化：app 传的是应用过 Shift/Caps 的 unicodeChar + Android metaState。
//     librime 会把 keycode 原样 PushInput（大写字符在拼音/笔画等中文方案下无法匹配），
//     且 kLockMask 会触发 ascii_composer 行为——中文方案统一归一化为小写并清除大小写修饰；
//     英文方案保留大小写（english schema 的 speller 按大小写匹配）
//  4. 联想词（getRimeAssociateList/selectRimeAssociate）：官方 librime 无此 API，
//     由 librime-predict 插件（PredictEngine）查询 predict.db 提供；动态学习：
//     基于 commit_history（分词后的提交词序列）增量统计 bigram（上屏词→下屏词），
//     落盘 user_predict.txt，查询时动态数据优先；app 层 CustomEngine 兜底。
//  5. setRimePageSize：no-op——候选页大小由重建后的 schema yaml 的 menu/page_size 配置
//  6. getRimeKeycodeByName：自建 X11 keysym 映射表（app 仅用 Page_Down/BackSpace）

#include <jni.h>

#include <rime_api.h>

// replaceRimeKey 需要访问 Session/Context（内部 API，与 librime 静态库同编，
// 不 include rime_api_impl.h 以避免与 rime_api.cc 重复定义）
#include <rime/service.h>
#include <rime/context.h>
#include <rime/commit_history.h>  // CommitRecord：联想提交补记 history
#include <rime/schema.h>  // AcquirePredictEngine 中访问 schema_id
#include <rime/key_table.h>  // kShiftMask/kLockMask 等修饰位

// 联想词：librime-predict 插件（PredictEngine + PredictDb，已合并编入 rime-static）
#include "predict_engine.h"
#include <rime/resource.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------

static RimeApi* g_api = nullptr;
static RimeSessionId g_session = 0;
static bool g_initialized = false;
static std::string g_schema_id;  // 当前 schema id（英文大小写判断用）

// 联想词状态（librime-predict）
static const rime::ResourceType kPredictDbResourceType = {"predict_db", "", ""};
static rime::PredictEngine* g_predict_engine = nullptr;  // 按 schema 缓存
static std::string g_predict_schema_id;
static std::vector<std::string> g_associate_words;  // 最近一次联想候选（app 可整体写回以对齐显示列表）
static int g_pending_associate = -1;  // 待提交的联想词索引（getRimeCommit 消费）
static std::vector<int> g_candidate_index_map;  // 显示位置 → rime 候选索引（setCandidateIndexMap 写入，-1 为 app 自定义项）

// 用户联想学习（对齐 fcitx5/libime 的 HistoryBigram 语义，轻量版）：
// 提交词序列（commit_history）→ 相邻词对 bigram（上屏词→下屏词）增量统计；
// 句子边界：librime 在 Return/BackSpace 时自动清空 history（天然断句），
// 另补超时（跨输入框/长停顿）与句末标点两种边界，防跨句串学；
// 容量上限 kMaxUserBigramEntries（落盘时按频率截断），落盘 user_predict.txt
static std::map<std::string, std::map<std::string, int>> g_user_bigrams;
static bool g_user_bigrams_dirty = false;
static int64_t g_last_user_bigrams_save = 0;  // 上次落盘时间（毫秒，节流用）
static int64_t g_last_commit_time = 0;  // 上次提交时间（毫秒，超时断句用）
static bool g_sentence_broken = false;  // 句末标点后置位：下一提交的 prev 失效
static const int64_t kSentenceTimeoutMs = 60000;  // 提交间隔超 60s 视为新句
static const size_t kMaxUserBigramEntries = 20000;  // 学习数据容量上限

// 前向声明：定义见下方“用户联想学习”区（startupRime/exitRime 先于定义使用）
static void LoadUserBigrams();
static void SaveUserBigrams();
static void LearnFromHistory(const rime::CommitHistory& history);
static void MaybeSaveUserBigrams();
static void ClearAssociationHistory();

// Java 数据类缓存（startup 时初始化）
struct JniCache {
  jclass Cls_RimeContext = nullptr;
  jclass Cls_RimeMenu = nullptr;
  jclass Cls_RimeComposition = nullptr;
  jclass Cls_CandidateListItem = nullptr;
  jclass Cls_RimeCommit = nullptr;
  jclass Cls_RimeStatus = nullptr;
  jmethodID Ctor_RimeContext = nullptr;
  jmethodID Ctor_RimeMenu = nullptr;
  jmethodID Ctor_RimeComposition = nullptr;
  jmethodID Ctor_CandidateListItem = nullptr;
  jmethodID Ctor_RimeCommit = nullptr;
  jmethodID Ctor_RimeStatus = nullptr;
};
static JniCache g_jni;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

static void InitJniCache(JNIEnv* env) {
  g_jni.Cls_RimeContext =
      (jclass)env->NewGlobalRef(env->FindClass("com/yuyan/inputmethod/core/RimeContext"));
  g_jni.Cls_RimeMenu =
      (jclass)env->NewGlobalRef(env->FindClass("com/yuyan/inputmethod/core/RimeMenu"));
  g_jni.Cls_RimeComposition =
      (jclass)env->NewGlobalRef(env->FindClass("com/yuyan/inputmethod/core/RimeComposition"));
  g_jni.Cls_CandidateListItem =
      (jclass)env->NewGlobalRef(env->FindClass("com/yuyan/inputmethod/core/CandidateListItem"));
  g_jni.Cls_RimeCommit =
      (jclass)env->NewGlobalRef(env->FindClass("com/yuyan/inputmethod/core/RimeCommit"));
  g_jni.Cls_RimeStatus =
      (jclass)env->NewGlobalRef(env->FindClass("com/yuyan/inputmethod/core/RimeStatus"));

  g_jni.Ctor_RimeContext = env->GetMethodID(
      g_jni.Cls_RimeContext, "<init>",
      "(Lcom/yuyan/inputmethod/core/RimeComposition;Lcom/yuyan/inputmethod/core/RimeMenu;"
      "Ljava/lang/String;[Ljava/lang/String;)V");
  g_jni.Ctor_RimeMenu = env->GetMethodID(
      g_jni.Cls_RimeMenu, "<init>",
      "(IIZII[Lcom/yuyan/inputmethod/core/CandidateListItem;)V");
  g_jni.Ctor_RimeComposition = env->GetMethodID(g_jni.Cls_RimeComposition, "<init>",
                                                "(IIIILjava/lang/String;)V");
  g_jni.Ctor_CandidateListItem = env->GetMethodID(g_jni.Cls_CandidateListItem, "<init>",
                                                  "(Ljava/lang/String;Ljava/lang/String;)V");
  g_jni.Ctor_RimeCommit =
      env->GetMethodID(g_jni.Cls_RimeCommit, "<init>", "(Ljava/lang/String;)V");
  g_jni.Ctor_RimeStatus = env->GetMethodID(
      g_jni.Cls_RimeStatus, "<init>",
      "(Ljava/lang/String;Ljava/lang/String;ZZZZZZZ)V");
}

// Android KeyEvent metaState → X11 修饰位（librime key_table.h）
// Android: META_SHIFT_ON=0x1 META_ALT_ON=0x2 META_CTRL_ON=0x1000
//          META_META_ON=0x10000 META_CAPS_LOCK_ON=0x100000
// X11:     kShiftMask=1<<0 kLockMask=1<<1 kControlMask=1<<2 kAltMask=1<<3 kMod4Mask=1<<6
static int ToRimeMask(int android_mask) {
  int m = 0;
  if (android_mask & 0x1) m |= kShiftMask;
  if (android_mask & 0x100000) m |= kLockMask;
  if (android_mask & 0x2) m |= kAltMask;
  if (android_mask & 0x1000) m |= kControlMask;
  if (android_mask & 0x10000) m |= kMod4Mask;
  return m;
}

// 英文方案：保留大小写进引擎；其余方案（拼音/T9/笔画/双拼）：归一化小写
static bool IsEnglishSchema() { return g_schema_id == "english"; }

static jstring ToJString(JNIEnv* env, const char* s) {
  return env->NewStringUTF(s ? s : "");
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_startupRime(JNIEnv* env, jclass,
                                                 jobject /*context*/,
                                                 jstring sharedDir,
                                                 jstring userDir,
                                                 jboolean /*fullCheck*/) {
  if (g_initialized) return;
  g_api = rime_get_api();
  if (!g_api) return;

  const char* shared = sharedDir ? env->GetStringUTFChars(sharedDir, nullptr) : nullptr;
  const char* user = userDir ? env->GetStringUTFChars(userDir, nullptr) : nullptr;

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = shared ? shared : "";
  traits.user_data_dir = user ? user : "";
  traits.app_name = "rime.yuyan";
  traits.distribution_name = "Yuyan";
  traits.distribution_code_name = "yuyan";
  traits.distribution_version = "1.0";
  traits.log_dir = "";  // 仅输出 logcat
  traits.min_log_level = 2;  // 仅 ERROR 及以上

  g_api->setup(&traits);
  g_api->initialize(&traits);
  // 不启动 maintenance（deploy）：assets 只有预编译 build/ 产物（prism/table + 展开的
  // schema.yaml），无 yaml 源词典，deploy 无意义且会失败；schema 直接加载 build/ 产物。

  if (shared) env->ReleaseStringUTFChars(sharedDir, shared);
  if (user) env->ReleaseStringUTFChars(userDir, user);

  InitJniCache(env);
  g_initialized = true;

  LoadUserBigrams();  // 加载历史学习数据（user_predict.txt）
}

extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_exitRime(JNIEnv* /*env*/, jclass) {
  if (!g_api) return;
  if (g_session != 0) {
    g_api->destroy_session(g_session);
    g_session = 0;
  }
  // Save while Service still owns user_data_dir; finalize releases that state.
  SaveUserBigrams();
  g_api->finalize();
  g_api = nullptr;
  g_initialized = false;
  g_schema_id.clear();
  g_user_bigrams.clear();
  g_user_bigrams_dirty = false;
  g_last_commit_time = 0;
  g_sentence_broken = false;
  delete g_predict_engine;
  g_predict_engine = nullptr;
  g_predict_schema_id.clear();
  g_associate_words.clear();
  g_pending_associate = -1;
  g_candidate_index_map.clear();
}

// 候选页大小由 schema yaml 的 menu/page_size 配置（重建 schema 时设为 100），no-op
extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_setRimePageSize(JNIEnv* /*env*/, jclass,
                                                     jint /*pageSize*/) {}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jboolean JNICALL
Java_com_yuyan_inputmethod_core_Rime_processRimeKey(JNIEnv* /*env*/, jclass,
                                                    jint keycode, jint mask) {
  if (!g_api || g_session == 0) return JNI_FALSE;
  if (keycode <= 0 || keycode == 0xffffff) return JNI_FALSE;

  int kc = keycode;
  int mk = ToRimeMask(mask);
  if (!IsEnglishSchema()) {
    // 中文方案：app 传的是应用过 Shift/Caps 的 unicodeChar（如 ⇧+J 传 'J'+SHIFT）。
    // librime 会把 keycode 原样 PushInput（大写字符在 a-z alphabet 下无法匹配），
    // kLockMask 还会触发 ascii_composer——归一化为小写并清除大小写修饰。
    if (kc >= 'A' && kc <= 'Z') kc += ('a' - 'A');
    mk &= ~(kShiftMask | kLockMask);
  }
  return g_api->process_key(g_session, kc, mk) ? JNI_TRUE : JNI_FALSE;
}

// 局部替换输入串（T9/乱序17 选择拼音后把拼音段替换为键码）。
// 官方 C API 无局部替换；set_input 不刷新候选，需补 RefreshNonConfirmedComposition。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_yuyan_inputmethod_core_Rime_replaceRimeKey(JNIEnv* env, jclass,
                                                    jint caretPos, jint length,
                                                    jstring key) {
  if (!g_initialized || g_session == 0) return JNI_FALSE;
  const char* keyStr = env->GetStringUTFChars(key, nullptr);
  if (!keyStr) return JNI_FALSE;

  bool ok = false;
  auto session = rime::Service::instance().GetSession(g_session);
  if (session) {
    rime::Context* ctx = session->context();
    if (ctx) {
      const std::string& input = ctx->input();
      if (caretPos >= 0 && caretPos <= (int)input.length()) {
        std::string newInput = input.substr(0, caretPos) + keyStr +
                               input.substr(caretPos + length);
        ctx->set_input(newInput);
        ctx->RefreshNonConfirmedComposition();
        ok = true;
      }
    }
  }
  env->ReleaseStringUTFChars(key, keyStr);
  return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_clearRimeComposition(JNIEnv* /*env*/, jclass) {
  if (!g_api || g_session == 0) return;
  g_api->clear_composition(g_session);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_yuyan_inputmethod_core_Rime_selectRimeCandidate(JNIEnv* /*env*/, jclass,
                                                         jint index) {
  if (!g_api || g_session == 0) return JNI_FALSE;
  // 显示位置 → rime 原始候选索引：app 更新候选时同步 setCandidateIndexMap/appendCandidateIndexMap，
  // 映射缺失时恒等兜底；-1 表示 app 自定义项（📋/echo 等），不经过引擎
  int real = index;
  if (index >= 0 && (size_t)index < g_candidate_index_map.size())
    real = g_candidate_index_map[index];
  if (real < 0) return JNI_FALSE;
  return g_api->select_candidate(g_session, (size_t)real) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_setRimeOption(JNIEnv* env, jclass,
                                                   jstring option, jboolean value) {
  if (!g_api || g_session == 0) return;
  const char* opt = env->GetStringUTFChars(option, nullptr);
  if (opt) {
    g_api->set_option(g_session, opt, value == JNI_TRUE);
    env->ReleaseStringUTFChars(option, opt);
  }
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

static void RecordExternalCommit(const std::string& text) {
  if (text.empty() || g_session == 0) return;
  if (auto session = rime::Service::instance().GetSession(g_session)) {
    if (auto* ctx = session->context()) {
      ctx->commit_history().Push(rime::CommitRecord{"raw", text});
      LearnFromHistory(ctx->commit_history());
      MaybeSaveUserBigrams();
    }
  }
}

// Editor-side deletes, sentence terminators, and input-field switches bypass
// librime's key handling. They must therefore explicitly end its history.
static void ClearAssociationHistory() {
  if (g_session != 0) {
    if (auto session = rime::Service::instance().GetSession(g_session)) {
      if (auto* ctx = session->context()) ctx->commit_history().clear();
    }
  }
  g_last_commit_time = 0;
  g_sentence_broken = false;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_yuyan_inputmethod_core_Rime_getRimeCommit(JNIEnv* env, jclass) {
  // 联想词提交：selectRimeAssociate 选中的预测词直接作为 commit 返回（一次性消费）
  if (g_pending_associate >= 0 && (size_t)g_pending_associate < g_associate_words.size()) {
    const std::string& word = g_associate_words[g_pending_associate];
    jstring text = ToJString(env, word.c_str());
    jobject obj = env->NewObject(g_jni.Cls_RimeCommit, g_jni.Ctor_RimeCommit, text);
    env->DeleteLocalRef(text);
    g_pending_associate = -1;
    // 联想提交不经过引擎（commit_history 不会更新）——手动补记并学习
    RecordExternalCommit(word);
    return obj;
  }
  if (!g_api || g_session == 0) return nullptr;
  RIME_STRUCT(RimeCommit, commit);
  if (!g_api->get_commit(g_session, &commit)) return nullptr;  // 消费式：取后引擎清空
  // 引擎提交已把本次文本 Push 进 commit_history（OnCommit 先 Push 再 sink）——学习 bigram
  if (commit.text && commit.text[0] != '\0') {
    if (g_session != 0) {
      if (auto session = rime::Service::instance().GetSession(g_session)) {
        if (auto* ctx = session->context()) LearnFromHistory(ctx->commit_history());
      }
    }
    MaybeSaveUserBigrams();
  }
  jstring text = ToJString(env, commit.text);
  jobject obj = env->NewObject(g_jni.Cls_RimeCommit, g_jni.Ctor_RimeCommit, text);
  env->DeleteLocalRef(text);
  g_api->free_commit(&commit);
  return obj;
}

extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_recordRimeExternalCommit(JNIEnv* env, jclass,
                                                              jstring text) {
  if (!text) return;
  const char* raw = env->GetStringUTFChars(text, nullptr);
  if (!raw) return;
  RecordExternalCommit(raw);
  env->ReleaseStringUTFChars(text, raw);
}

extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_clearRimeAssociationHistory(
    JNIEnv* /*env*/, jclass) {
  ClearAssociationHistory();
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_yuyan_inputmethod_core_Rime_getRimeContext(JNIEnv* env, jclass) {
  if (!g_api || g_session == 0) return nullptr;
  RIME_STRUCT(RimeContext, ctx);
  if (!g_api->get_context(g_session, &ctx)) return nullptr;

  // composition
  jstring preedit = ToJString(env, ctx.composition.preedit);
  jobject composition = env->NewObject(
      g_jni.Cls_RimeComposition, g_jni.Ctor_RimeComposition, ctx.composition.length,
      ctx.composition.cursor_pos, ctx.composition.sel_start, ctx.composition.sel_end,
      preedit);
  env->DeleteLocalRef(preedit);

  // menu + candidates
  int numCandidates = ctx.menu.num_candidates;
  jobjectArray candidates =
      env->NewObjectArray(numCandidates, g_jni.Cls_CandidateListItem, nullptr);
  for (int i = 0; i < numCandidates; ++i) {
    jstring text = ToJString(env, ctx.menu.candidates[i].text);
    jstring comment = ToJString(env, ctx.menu.candidates[i].comment);
    jobject cand = env->NewObject(g_jni.Cls_CandidateListItem,
                                  g_jni.Ctor_CandidateListItem, comment, text);
    env->SetObjectArrayElement(candidates, i, cand);
    env->DeleteLocalRef(text);
    env->DeleteLocalRef(comment);
    env->DeleteLocalRef(cand);
  }
  jobject menu = env->NewObject(
      g_jni.Cls_RimeMenu, g_jni.Ctor_RimeMenu, ctx.menu.page_size, ctx.menu.page_no,
      ctx.menu.is_last_page ? JNI_TRUE : JNI_FALSE, ctx.menu.highlighted_candidate_index,
      numCandidates, candidates);
  env->DeleteLocalRef(candidates);

  // select_labels：app 不使用，给空数组
  jclass strCls = env->FindClass("java/lang/String");
  jobjectArray selectLabels = env->NewObjectArray(0, strCls, nullptr);
  env->DeleteLocalRef(strCls);

  jstring preview = ToJString(env, ctx.commit_text_preview);
  jobject result = env->NewObject(g_jni.Cls_RimeContext, g_jni.Ctor_RimeContext,
                                  composition, menu, preview, selectLabels);
  env->DeleteLocalRef(preview);
  env->DeleteLocalRef(selectLabels);
  env->DeleteLocalRef(menu);
  env->DeleteLocalRef(composition);

  g_api->free_context(&ctx);
  return result;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_yuyan_inputmethod_core_Rime_getRimeStatus(JNIEnv* env, jclass) {
  if (!g_api || g_session == 0) return nullptr;
  RIME_STRUCT(RimeStatus, status);
  if (!g_api->get_status(g_session, &status)) return nullptr;

  jstring schemaId = ToJString(env, status.schema_id);
  jstring schemaName = ToJString(env, status.schema_name);
  jobject obj = env->NewObject(
      g_jni.Cls_RimeStatus, g_jni.Ctor_RimeStatus, schemaId, schemaName,
      status.is_disabled ? JNI_TRUE : JNI_FALSE,
      status.is_composing ? JNI_TRUE : JNI_FALSE,
      status.is_ascii_mode ? JNI_TRUE : JNI_FALSE,
      status.is_full_shape ? JNI_TRUE : JNI_FALSE,
      status.is_simplified ? JNI_TRUE : JNI_FALSE,
      status.is_traditional ? JNI_TRUE : JNI_FALSE,
      status.is_ascii_punct ? JNI_TRUE : JNI_FALSE);
  env->DeleteLocalRef(schemaId);
  env->DeleteLocalRef(schemaName);

  g_api->free_status(&status);
  return obj;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_yuyan_inputmethod_core_Rime_getCurrentRimeSchema(JNIEnv* env, jclass) {
  if (!g_api || g_session == 0) return ToJString(env, "");
  char buf[128] = {0};
  g_api->get_current_schema(g_session, buf, sizeof(buf));
  return ToJString(env, buf);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_yuyan_inputmethod_core_Rime_selectRimeSchema(JNIEnv* env, jclass,
                                                      jstring schemaId) {
  if (!g_api || !g_initialized) return JNI_FALSE;
  const char* id = env->GetStringUTFChars(schemaId, nullptr);
  if (!id) return JNI_FALSE;

  if (g_session == 0) {
    g_session = g_api->create_session();
    if (g_session == 0) {
      env->ReleaseStringUTFChars(schemaId, id);
      return JNI_FALSE;
    }
  }
  bool ok = g_api->select_schema(g_session, id);
  if (ok) g_schema_id = id;
  env->ReleaseStringUTFChars(schemaId, id);
  return ok ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------
// 按键名称 → X11 keysym（app 仅使用 Page_Down/BackSpace，多映射几个备用）
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jint JNICALL
Java_com_yuyan_inputmethod_core_Rime_getRimeKeycodeByName(JNIEnv* env, jclass,
                                                          jstring name) {
  const char* n = env->GetStringUTFChars(name, nullptr);
  if (!n) return 0;
  int code = 0;
  if (std::strcmp(n, "Page_Down") == 0) code = 0xff56;  // XK_Page_Down
  else if (std::strcmp(n, "Page_Up") == 0) code = 0xff55;
  else if (std::strcmp(n, "BackSpace") == 0) code = 0xff08;
  else if (std::strcmp(n, "Return") == 0) code = 0xff0d;
  else if (std::strcmp(n, "Tab") == 0) code = 0xff09;
  else if (std::strcmp(n, "space") == 0) code = 0x20;
  else if (std::strcmp(n, "Escape") == 0) code = 0xff1b;
  else if (std::strcmp(n, "Delete") == 0) code = 0xffff;
  else if (std::strcmp(n, "Home") == 0) code = 0xff50;
  else if (std::strcmp(n, "End") == 0) code = 0xff57;
  else if (std::strcmp(n, "Left") == 0) code = 0xff51;
  else if (std::strcmp(n, "Right") == 0) code = 0xff53;
  else if (std::strcmp(n, "Up") == 0) code = 0xff52;
  else if (std::strcmp(n, "Down") == 0) code = 0xff54;
  else if (std::strcmp(n, "Shift_L") == 0) code = 0xffe1;
  else if (std::strcmp(n, "Shift_R") == 0) code = 0xffe2;
  else if (std::strcmp(n, "Control_L") == 0) code = 0xffe3;
  else if (std::strcmp(n, "Control_R") == 0) code = 0xffe4;
  else if (std::strcmp(n, "Alt_L") == 0) code = 0xffe9;
  else if (std::strcmp(n, "Alt_R") == 0) code = 0xffea;
  else if (n[0] != '\0' && n[1] == '\0') code = (unsigned char)n[0];  // 单字符 ASCII
  env->ReleaseStringUTFChars(name, n);
  return code;
}

// ---------------------------------------------------------------------------
// 用户联想学习（借鉴 fcitx5/libime 的 UserLanguageModel 思路，轻量 bigram 版）
// ---------------------------------------------------------------------------

// user_predict.txt 路径：用户数据目录（与 predict.db 同目录）
static std::string UserPredictPath() {
  return (rime::Service::instance().deployer().user_data_dir / "user_predict.txt").string();
}

// 汉字开头（UTF-8 首字节 E4~E9 覆盖常用汉字区）——只学中文词对，过滤英文/标点
static bool StartsWithHan(const std::string& s) {
  if (s.empty()) return false;
  unsigned char c = (unsigned char)s[0];
  return c >= 0xE4 && c <= 0xE9;
}

static int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 句末标点（。！？；…）——UTF-8 尾字节匹配
static bool EndsSentence(const std::string& s) {
  if (s.size() < 3) return false;
  const size_t n = s.size();
  const char* t = s.c_str() + n - 3;
  if (std::memcmp(t, "\xE3\x80\x82", 3) == 0) return true;  // 。
  if (std::memcmp(t, "\xEF\xBC\x81", 3) == 0) return true;  // ！
  if (std::memcmp(t, "\xEF\xBC\x9F", 3) == 0) return true;  // ？
  if (std::memcmp(t, "\xEF\xBC\x9B", 3) == 0) return true;  // ；
  if (std::memcmp(t, "\xE2\x80\xA6", 3) == 0) return true;  // …
  return false;
}

static void LoadUserBigrams() {
  g_user_bigrams.clear();
  std::ifstream in(UserPredictPath());
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    std::string key = line.substr(0, t1);
    std::string value = line.substr(t1 + 1, t2 - t1 - 1);
    int count = atoi(line.c_str() + t2 + 1);
    if (key.empty() || value.empty() || count <= 0) continue;
    g_user_bigrams[key][value] += count;
  }
}

static void SaveUserBigrams() {
  if (!g_user_bigrams_dirty) return;
  // 容量控制：总对数超限时按频率降序保留前 kMaxUserBigramEntries（防无限增长）
  size_t total = 0;
  for (const auto& kv : g_user_bigrams) total += kv.second.size();
  if (total > kMaxUserBigramEntries) {
    // (count, value, key) 全量收集后排序截断，再重建 map
    std::vector<std::pair<int, std::pair<std::string, std::string>>> all;
    all.reserve(total);
    for (const auto& kv : g_user_bigrams) {
      for (const auto& vc : kv.second) {
        all.emplace_back(vc.second, std::make_pair(vc.first, kv.first));
      }
    }
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    all.resize(kMaxUserBigramEntries);
    g_user_bigrams.clear();
    for (const auto& e : all) g_user_bigrams[e.second.second][e.second.first] = e.first;
  }
  std::ofstream out(UserPredictPath());
  if (!out) return;
  for (const auto& kv : g_user_bigrams) {
    for (const auto& vc : kv.second) {
      out << kv.first << '\t' << vc.first << '\t' << vc.second << '\n';
    }
  }
  g_user_bigrams_dirty = false;
}

// 落盘节流：距上次保存超过 60s 才写（查询/提交路径频繁，避免每击必写）
static void MaybeSaveUserBigrams() {
  int64_t now = NowMs();
  if (now - g_last_user_bigrams_save < 60000) return;
  g_last_user_bigrams_save = now;
  SaveUserBigrams();
}

// 从 commit_history（librime 分词后的提交词序列）学习：最后两个有效记录 → bigram。
// 边界处理：① Return/BackSpace 由 librime 清空 history（天然断句，prev 自动失效）
// ② 提交间隔超时（跨输入框/长停顿）视为新句 ③ 句末标点后置 g_sentence_broken
static void LearnFromHistory(const rime::CommitHistory& history) {
  int64_t now = NowMs();
  bool timeout = (g_last_commit_time > 0 && now - g_last_commit_time > kSentenceTimeoutMs);
  g_last_commit_time = now;
  std::string prev, cur;
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    if (it->type == "thru" || it->text.empty()) continue;  // 跳过按键直通记录
    if (cur.empty()) {
      cur = it->text;
    } else {
      prev = it->text;
      break;
    }
  }
  if (cur.empty()) return;
  // 超时断句或上句以句末标点结束：prev 失效，只记当前词
  if (timeout || g_sentence_broken) {
    g_sentence_broken = false;
    prev.clear();
  }
  if (!prev.empty() && StartsWithHan(prev) && StartsWithHan(cur)) {
    g_user_bigrams[prev][cur]++;
    g_user_bigrams_dirty = true;
  }
  // 句末标点结尾：后续提交不再与当前词成对
  if (EndsSentence(cur)) g_sentence_broken = true;
}

// ---------------------------------------------------------------------------
// 联想词：官方 librime 无联想 API（getRimeAssociateList/selectRimeAssociate 是
// yuyan 定制引擎的私有扩展）。由 librime-predict 插件提供：PredictEngine 查询
// predict.db（用户/共享数据目录，精确匹配；查不到时按 UTF-8 码点做后缀回退）。
// 动态学习数据（user_predict.txt）优先，静态 predict.db 补充，去重合并。
// 选中候选后由 getRimeCommit 以 pending 方式直接返回预测词，app 层提交上屏。
// ---------------------------------------------------------------------------

// 按当前 schema 获取/缓存 PredictEngine（predict.db 加载失败返回 nullptr）
static rime::PredictEngine* AcquirePredictEngine() {
  if (!g_initialized || g_session == 0) return nullptr;
  auto session = rime::Service::instance().GetSession(g_session);
  if (!session || !session->schema()) return nullptr;
  const std::string schema_id = session->schema()->schema_id();
  if (g_predict_engine && g_predict_schema_id == schema_id) return g_predict_engine;
  // schema 变化（或首次）：重建引擎（user 目录优先，shared 目录兜底）
  delete g_predict_engine;
  g_predict_engine = nullptr;
  g_predict_schema_id.clear();
  rime::FallbackResourceResolver resolver(kPredictDbResourceType);
  resolver.set_root_path(rime::Service::instance().deployer().user_data_dir);
  resolver.set_fallback_root_path(rime::Service::instance().deployer().shared_data_dir);
  rime::path db_path = resolver.ResolvePath("predict.db");
  auto db = rime::New<rime::PredictDb>(db_path);
  if (!db->Load()) return nullptr;
  // max_iterations=0 不限连续预测次数；max_candidates=0 返回全部候选（app 层再截取）
  g_predict_engine = new rime::PredictEngine(db, 0, 0);
  g_predict_schema_id = schema_id;
  return g_predict_engine;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_yuyan_inputmethod_core_Rime_getRimeAssociateList(JNIEnv* env, jclass,
                                                          jstring key) {
  jclass strCls = env->FindClass("java/lang/String");
  g_associate_words.clear();
  if (key) {
    const char* text = env->GetStringUTFChars(key, nullptr);
    if (text) {
      rime::PredictEngine* engine = AcquirePredictEngine();
      std::string query = text;
      // 预测 key 为词级（如“就”“今天”），光标前文本可能是完整句子——
      // 整串精确匹配失败时按 UTF-8 码点去掉前缀字符逐级回退（如“我们今天就”→“就”）；
      // 每级先查动态学习数据（用户习惯优先，按出现次数降序），再查静态 predict.db 补充
      while (!query.empty()) {
        bool hit = false;
        // ① 动态：用户 bigram 学习数据（最多 5 条，保持与候选栏显示量一致）
        auto dit = g_user_bigrams.find(query);
        if (dit != g_user_bigrams.end()) {
          std::vector<std::pair<std::string, int>> items(dit->second.begin(),
                                                         dit->second.end());
          std::sort(items.begin(), items.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });
          for (const auto& iv : items) {
            g_associate_words.push_back(iv.first);
            if (g_associate_words.size() >= 5) break;
          }
          hit = true;
        }
        // ② 静态：predict.db（补充动态未覆盖的词，去重，总上限 10）
        if (engine && engine->Predict(nullptr, query)) {
          int n = engine->num_candidates();
          for (int i = 0; i < n; ++i) {
            const std::string w = engine->candidate(i);
            if (std::find(g_associate_words.begin(), g_associate_words.end(), w) ==
                g_associate_words.end()) {
              g_associate_words.push_back(w);
              if (g_associate_words.size() >= 10) break;
            }
          }
          hit = true;
        }
        if (hit) break;
        // 去掉第一个 UTF-8 码点后继续回退
        size_t first = 1;
        unsigned char c = (unsigned char)query[0];
        if (c >= 0xF0) first = 4;
        else if (c >= 0xE0) first = 3;
        else if (c >= 0xC0) first = 2;
        if (first >= query.size()) break;
        query = query.substr(first);
      }
      env->ReleaseStringUTFChars(key, text);
    }
  }
  jobjectArray arr =
      env->NewObjectArray((jsize)g_associate_words.size(), strCls, nullptr);
  for (size_t i = 0; i < g_associate_words.size(); ++i) {
    jstring s = ToJString(env, g_associate_words[i].c_str());
    env->SetObjectArrayElement(arr, (jsize)i, s);
    env->DeleteLocalRef(s);
  }
  env->DeleteLocalRef(strCls);
  return arr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_yuyan_inputmethod_core_Rime_selectRimeAssociate(JNIEnv* /*env*/, jclass,
                                                         jint index) {
  if (index < 0 || (size_t)index >= g_associate_words.size()) return JNI_FALSE;
  g_pending_associate = index;  // 下次 getRimeCommit 时直接返回该预测词
  return JNI_TRUE;
}

// 联想词表整体写回：app 拼好的最终显示列表（含标点/日期等自定义项）与选择索引对齐，
// selectRimeAssociate(index) 按下标取词即得显示文本，杜绝 Kotlin 拼接与 JNI 词表错位
extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_setAssociateWords(JNIEnv* env, jclass,
                                                       jobjectArray words) {
  g_associate_words.clear();
  g_pending_associate = -1;
  if (!words) return;
  jsize len = env->GetArrayLength(words);
  g_associate_words.reserve((size_t)len);
  for (jsize i = 0; i < len; ++i) {
    jstring s = (jstring)env->GetObjectArrayElement(words, i);
    if (!s) continue;
    const char* utf = env->GetStringUTFChars(s, nullptr);
    if (utf) {
      g_associate_words.emplace_back(utf);
      env->ReleaseStringUTFChars(s, utf);
    }
    env->DeleteLocalRef(s);
  }
}

// 候选索引映射（整体替换）：显示位置 → rime 原始候选索引，-1 为 app 自定义项（📋/echo）
extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_setCandidateIndexMap(JNIEnv* env, jclass,
                                                          jintArray map) {
  g_candidate_index_map.clear();
  if (!map) return;
  jsize len = env->GetArrayLength(map);
  g_candidate_index_map.reserve((size_t)len);
  const jint* elems = env->GetIntArrayElements(map, nullptr);
  if (elems) {
    for (jsize i = 0; i < len; ++i) g_candidate_index_map.push_back(elems[i]);
    env->ReleaseIntArrayElements(map, const_cast<jint*>(elems), JNI_ABORT);
  }
}

// 候选索引映射（追加）：与 DecodingInfo 把下一页候选追加到显示列表的顺序保持一致
extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_appendCandidateIndexMap(JNIEnv* env, jclass,
                                                             jintArray map) {
  if (!map) return;
  jsize len = env->GetArrayLength(map);
  const jint* elems = env->GetIntArrayElements(map, nullptr);
  if (elems) {
    for (jsize i = 0; i < len; ++i) g_candidate_index_map.push_back(elems[i]);
    env->ReleaseIntArrayElements(map, const_cast<jint*>(elems), JNI_ABORT);
  }
}
