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
//     返回空；联想由 app 层 CustomEngine 提供
//  5. setRimePageSize：no-op——候选页大小由重建后的 schema yaml 的 menu/page_size 配置
//  6. getRimeKeycodeByName：自建 X11 keysym 映射表（app 仅用 Page_Down/BackSpace）

#include <jni.h>

#include <rime_api.h>

// replaceRimeKey 需要访问 Session/Context（内部 API，与 librime 静态库同编，
// 不 include rime_api_impl.h 以避免与 rime_api.cc 重复定义）
#include <rime/service.h>
#include <rime/context.h>
#include <rime/key_table.h>  // kShiftMask/kLockMask 等修饰位

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------

static RimeApi* g_api = nullptr;
static RimeSessionId g_session = 0;
static bool g_initialized = false;
static std::string g_schema_id;  // 当前 schema id（英文大小写判断用）

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
}

extern "C" JNIEXPORT void JNICALL
Java_com_yuyan_inputmethod_core_Rime_exitRime(JNIEnv* /*env*/, jclass) {
  if (!g_api) return;
  if (g_session != 0) {
    g_api->destroy_session(g_session);
    g_session = 0;
  }
  g_api->finalize();
  g_api = nullptr;
  g_initialized = false;
  g_schema_id.clear();
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
  return g_api->select_candidate(g_session, (size_t)index) ? JNI_TRUE : JNI_FALSE;
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

extern "C" JNIEXPORT jobject JNICALL
Java_com_yuyan_inputmethod_core_Rime_getRimeCommit(JNIEnv* env, jclass) {
  if (!g_api || g_session == 0) return nullptr;
  RIME_STRUCT(RimeCommit, commit);
  if (!g_api->get_commit(g_session, &commit)) return nullptr;  // 消费式：取后引擎清空
  jstring text = ToJString(env, commit.text);
  jobject obj = env->NewObject(g_jni.Cls_RimeCommit, g_jni.Ctor_RimeCommit, text);
  env->DeleteLocalRef(text);
  g_api->free_commit(&commit);
  return obj;
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
// 联想词：官方 librime 无联想 API（getRimeAssociateList/selectRimeAssociate 是
// yuyan 定制引擎的私有扩展）。返回空/失败，联想词由 app 层 CustomEngine 提供。
// ---------------------------------------------------------------------------

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_yuyan_inputmethod_core_Rime_getRimeAssociateList(JNIEnv* env, jclass,
                                                          jstring /*key*/) {
  jclass strCls = env->FindClass("java/lang/String");
  jobjectArray arr = env->NewObjectArray(0, strCls, nullptr);
  env->DeleteLocalRef(strCls);
  return arr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_yuyan_inputmethod_core_Rime_selectRimeAssociate(JNIEnv* /*env*/, jclass,
                                                         jint /*index*/) {
  return JNI_FALSE;
}
