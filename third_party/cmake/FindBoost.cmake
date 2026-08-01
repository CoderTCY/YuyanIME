# SPDX-FileCopyrightText: 2015 - 2024 Rime community
#
# SPDX-License-Identifier: GPL-3.0-or-later

# 最小化 FindBoost：librime 在非 Linux 分支 find_package(Boost) 无 COMPONENTS，
# 仅 header-only 使用（不链接任何 boost 库）。聚合头由 Boost.cmake 在 configure
# 时准备到构建目录（${CMAKE_BINARY_DIR}/boost-headers），此处只提供 include 路径。

set(Boost_FOUND TRUE)
set(Boost_VERSION "1.89.0")
set(Boost_LIBRARIES "")
set(Boost_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/boost-headers")
message(STATUS "Yuyan FindBoost executed: FOUND=${Boost_FOUND} VERSION=${Boost_VERSION} INCLUDE=${Boost_INCLUDE_DIRS}")
