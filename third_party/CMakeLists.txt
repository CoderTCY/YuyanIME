# yuyanime-rime：librime 源码集成构建（NDK + CMake）
# 参考 osfans/trime 的 jni 构建组织（https://github.com/osfans/trime）
# 产物：libyuyanime.so（JNI 桥，保持 com.yuyan.inputmethod.core.Rime 的 native 签名不变）

cmake_minimum_required(VERSION 3.18.0)
project(yuyanime-rime CXX C)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# leveldb 以 PUBLIC -Werror 编译选项传播到链接方，opencc/librime 的
# C++17 弃用警告（std::iterator 等）会被提升为错误——解除该警告的 error 提升
add_compile_options(-Wno-error=deprecated-declarations)

# 通用选项
option(BUILD_SHARED_LIBS "" OFF)
option(BUILD_TESTING "" OFF)

# Boost：librime 仅 header-only 使用（官方 Windows 路径同样不编译 boost 库）——
# include(Boost) 在 configure 时自动下载/提取聚合头到构建目录（缓存项目外，不进仓库）
include(Boost)
# 全局 include 聚合头（librime 的 include_directories 是目录属性，不传播到 jni/ 子目录）
include_directories("${CMAKE_BINARY_DIR}/boost-headers")

option(WITH_GFLAGS "Use gflags" OFF)
option(WITH_GTEST "Use googletest" OFF)
option(WITH_UNWIND "Enable libunwind support" OFF)
add_subdirectory(librime/deps/glog)

option(YAML_CPP_BUILD_TOOLS "" OFF)
option(YAML_CPP_BUILD_TESTS "" OFF)
option(YAML_CPP_INSTALL "" OFF)
add_subdirectory(librime/deps/yaml-cpp)

# leveldb 压缩支持
option(SNAPPY_BUILD_TESTS "" OFF)
option(SNAPPY_BUILD_BENCHMARKS "" OFF)
option(SNAPPY_INSTALL "" OFF)
add_subdirectory(snappy)
set(HAVE_SNAPPY TRUE)

option(LEVELDB_BUILD_TESTS "" OFF)
option(LEVELDB_BUILD_BENCHMARKS "" OFF)
option(LEVELDB_INSTALL "" OFF)
add_subdirectory(librime/deps/leveldb)

add_subdirectory(librime/deps/marisa-trie)

set(LIBMARISA marisa)
option(USE_SYSTEM_MARISA "" ON)
add_subdirectory(librime/deps/opencc)
include(OpenccWorkarounds)

# X11 keysym 头（librime 依赖）
set(X11Keysym "${CMAKE_CURRENT_SOURCE_DIR}/librime/include")
include_directories("${X11Keysym}")

# librime-predict 依赖 darts-clone（header-only）：必须在 add_subdirectory(librime)
# 之前声明，插件源码（librime/plugins/ 下编译）才能继承该 include 路径
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/darts-clone")

# librime 本体：静态库 rime-static（外部插件经 librime/plugins 机制编入；
# 联想插件 librime-predict 由 build-librime.ps1 以 junction 挂到 librime/plugins/ 下）
option(BUILD_TEST "" OFF)
option(BUILD_STATIC "" ON)
option(ENABLE_EXTERNAL_PLUGINS "" OFF)
add_subdirectory(librime)

# JNI 桥（产出 libyuyanime.so）
add_subdirectory(jni)
