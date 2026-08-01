# Boost 构建时准备：librime 仅 header-only 使用 boost（官方 Windows 路径同样不编译
# boost 库），只需聚合头目录 boost/。
#
# 设计（不把 boost 放进项目/仓库）：
#   1. 缓存目录在项目外：%LOCALAPPDATA%\yuyanime-boost-cache（跨 clean 持久）
#   2. 下载完整版 boost 源码包（archives.boost.io，官方源）——缓存存在则跳过下载
#   3. 只提取聚合头 boost/（~250MB）到构建目录 ${CMAKE_BINARY_DIR}/boost-headers
#      （构建目录已被 .gitignore 排除，不进仓库）
#   4. 清理源码包（保留缓存）
# 下载走 libcurl（file(DOWNLOAD)），代理通过环境变量 https_proxy/http_proxy 生效。

set(BOOST_VERSION 1.89.0)
string(REPLACE "." "_" BOOST_VERSION_UNDERSCORE ${BOOST_VERSION})
set(BOOST_TARBALL "boost-${BOOST_VERSION}.tar.gz")
set(BOOST_URL
    "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz")
set(BOOST_HEADERS_ROOT "${CMAKE_BINARY_DIR}/boost-headers")

if(NOT EXISTS "${BOOST_HEADERS_ROOT}/boost/regex.hpp")
  message(STATUS "Preparing Boost ${BOOST_VERSION} headers (one-time, ~250MB) ...")

  # 项目外缓存目录
  set(BOOST_CACHE_DIR "$ENV{LOCALAPPDATA}/yuyanime-boost-cache")
  set(BOOST_TARBALL_CACHE "${BOOST_CACHE_DIR}/${BOOST_TARBALL}")

  if(EXISTS "${BOOST_TARBALL_CACHE}")
    message(STATUS "Boost: using cached tarball ${BOOST_TARBALL_CACHE}")
    set(BOOST_TARBALL_PATH "${BOOST_TARBALL_CACHE}")
  else()
    message(STATUS "Boost: downloading ${BOOST_URL}")
    file(DOWNLOAD "${BOOST_URL}" "${BOOST_CACHE_DIR}/${BOOST_TARBALL}"
         SHOW_PROGRESS STATUS boost_dl_status)
    list(GET boost_dl_status 0 boost_dl_code)
    if(NOT boost_dl_code EQUAL 0)
      list(GET boost_dl_status 1 boost_dl_msg)
      message(FATAL_ERROR "Boost download failed: ${boost_dl_msg}")
    endif()
    set(BOOST_TARBALL_PATH "${BOOST_CACHE_DIR}/${BOOST_TARBALL}")
  endif()

  # 只提取聚合头目录 boost/（tar 支持指定路径）
  file(MAKE_DIRECTORY "${BOOST_HEADERS_ROOT}")
  execute_process(
    COMMAND tar -xf "${BOOST_TARBALL_PATH}" -C "${CMAKE_BINARY_DIR}"
            "boost_${BOOST_VERSION_UNDERSCORE}/boost"
    RESULT_VARIABLE boost_tar_result)
  if(NOT boost_tar_result EQUAL 0)
    message(FATAL_ERROR "Failed to extract boost headers from ${BOOST_TARBALL_PATH}")
  endif()
  file(RENAME "${CMAKE_BINARY_DIR}/boost_${BOOST_VERSION_UNDERSCORE}/boost"
       "${BOOST_HEADERS_ROOT}/boost")
  message(STATUS "Boost headers ready at ${BOOST_HEADERS_ROOT}/boost")
endif()

# 供 FindBoost.cmake 使用（librime 的 find_package(Boost) 在子目录中调用）
set(Boost_FOUND TRUE)
set(Boost_VERSION "${BOOST_VERSION}")
set(Boost_LIBRARIES "")
set(Boost_INCLUDE_DIRS "${BOOST_HEADERS_ROOT}")
