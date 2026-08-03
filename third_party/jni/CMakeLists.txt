# JNI 桥：librime.so（引擎为原版 librime，名称名副其实）
# target 名 rime → CMake 自动加 lib 前缀产出 librime.so；
# 保持 com.yuyan.inputmethod.core.Rime 的 16 个 native 方法签名，
# Java 层仅 System.loadLibrary("rime")；引擎主体为 librime 静态库 rime-static。
add_library(rime SHARED jni_bridge.cc)
target_link_libraries(rime rime-static ${Opencc_LIBRARY})
target_include_directories(rime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../librime/src"
  "${CMAKE_BINARY_DIR}/librime/src"
  "${Opencc_INCLUDE_PATH}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../librime/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../librime-predict/src"  # 联想插件（PredictEngine）
  "${CMAKE_CURRENT_SOURCE_DIR}/../darts-clone")  # librime-predict 依赖（header-only）
