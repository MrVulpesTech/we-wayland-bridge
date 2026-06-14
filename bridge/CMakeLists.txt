# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 we-wayland-bridge contributors
#
# Builds the Stage A frame-producer host. It links the prebuilt
# linux-wallpaperengine-core shared library (built from the pinned next-v2
# worktree by build.sh) and the public C headers from that worktree.

cmake_minimum_required(VERSION 3.16)
project(we-wayland-bridge-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locations of the prebuilt core and its source tree (the pinned worktree).
# Overridable, but the defaults match build.sh's layout.
set(CORE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/core"
    CACHE PATH "Directory where linux-wallpaperengine-core was built")
set(CORE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../next-v2-review"
    CACHE PATH "Pinned next-v2 worktree (for include/ and stb)")

find_library(CORE_LIB
    NAMES linux-wallpaperengine-core
    PATHS "${CORE_BUILD_DIR}/lib"
    NO_DEFAULT_PATH REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
# Stage C: gbm allocates the dma-buf-backed output buffer; libdrm provides
# drm_fourcc.h (DRM_FORMAT_* and modifier constants).
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(wpe-host host/main.cpp)

target_include_directories(wpe-host PRIVATE
    "${CORE_SRC_DIR}/include"              # public wp_* C API
    "${CORE_SRC_DIR}/src/External/stb")    # stb_image_write.h (public domain)

target_link_libraries(wpe-host PRIVATE
    "${CORE_LIB}"
    PkgConfig::EGL
    PkgConfig::PIPEWIRE
    PkgConfig::GBM
    PkgConfig::DRM
    GLEW::GLEW
    OpenGL::GL)

# Find the core .so at runtime without installing it.
set_target_properties(wpe-host PROPERTIES
    BUILD_RPATH "${CORE_BUILD_DIR}/lib")
