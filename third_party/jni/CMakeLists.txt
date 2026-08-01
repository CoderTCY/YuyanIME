# JNI 桥：libyuyanime.so
# 保持 com.yuyan.inputmethod.core.Rime 的 16 个 native 方法签名，
# Java 层零改动；引擎主体为官方 librime（静态库 rime-static）。
add_library(yuyanime SHARED jni_bridge.cc)
target_link_libraries(yuyanime rime-static ${Opencc_LIBRARY})
target_include_directories(yuyanime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../librime/src"
  "${CMAKE_BINARY_DIR}/librime/src"
  "${Opencc_INCLUDE_PATH}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../librime/include")
