cmake_minimum_required(VERSION 3.21)

project(DualIC4VarjoApp_AdaptiveGazeGuidance
    VERSION 0.1.0
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This application requires Windows, Direct3D 12, IC Imaging Control 4, and Varjo Runtime.")
endif()

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Do not update populated dependencies automatically" FORCE)

# Explicit dependency versions requested by the application.
set(DUAL_IC4_VARJO_IC4EXT_TAG "v1.0.1")
set(DUAL_IC4_VARJO_VARJOXR_TAG "v0.1.0")
set(DUAL_IC4_VARJO_D3D12HELPER_TAG "v1.12.1")

# -----------------------------------------------------------------------------
# IC Imaging Control 4 SDK discovery and validation
# -----------------------------------------------------------------------------
# IC4Ext expects this directory layout:
#   <IC4_SDK_ROOT>/include/ic4/ic4.h
#   <IC4_SDK_ROOT>/lib/x64/ic4core.lib
#   <IC4_SDK_ROOT>/bin/x64/ic4core.dll
#
# A previous build could continue with an empty IC4_SDK_ROOT and fail later with
# C1083 at IC4ChunkMetadata.hpp. Resolve and validate the SDK before IC4Ext is
# configured so the error is reported at CMake configure time instead.
set(IC4_SDK_ROOT "" CACHE PATH "Path to the IC Imaging Control 4 SDK root")

function(dual_ic4_varjo_normalize_ic4_root candidate out_root)
    if(NOT candidate)
        set(${out_root} "" PARENT_SCOPE)
        return()
    endif()

    file(TO_CMAKE_PATH "${candidate}" normalized)
    string(REGEX REPLACE "/+$" "" normalized "${normalized}")

    if(EXISTS "${normalized}/include/ic4/ic4.h")
        set(${out_root} "${normalized}" PARENT_SCOPE)
        return()
    endif()

    # Also accept the include directory itself, for example:
    #   -DIC4_SDK_ROOT=C:/.../IC Imaging Control 4/include
    if(EXISTS "${normalized}/ic4/ic4.h")
        get_filename_component(parent "${normalized}" DIRECTORY)
        set(${out_root} "${parent}" PARENT_SCOPE)
        return()
    endif()

    set(${out_root} "" PARENT_SCOPE)
endfunction()

set(_dual_ic4_sdk_candidates)
if(IC4_SDK_ROOT)
    list(APPEND _dual_ic4_sdk_candidates "${IC4_SDK_ROOT}")
endif()
if(DEFINED ENV{IC4_SDK_ROOT} AND NOT "$ENV{IC4_SDK_ROOT}" STREQUAL "")
    list(APPEND _dual_ic4_sdk_candidates "$ENV{IC4_SDK_ROOT}")
endif()
if(DEFINED ENV{IC4PATH} AND NOT "$ENV{IC4PATH}" STREQUAL "")
    list(APPEND _dual_ic4_sdk_candidates "$ENV{IC4PATH}")
endif()
if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
    list(APPEND _dual_ic4_sdk_candidates
        "$ENV{LOCALAPPDATA}/Programs/The Imaging Source Europe GmbH/IC Imaging Control 4")
endif()
if(DEFINED ENV{ProgramFiles} AND NOT "$ENV{ProgramFiles}" STREQUAL "")
    list(APPEND _dual_ic4_sdk_candidates
        "$ENV{ProgramFiles}/The Imaging Source Europe GmbH/IC Imaging Control 4")
endif()
list(APPEND _dual_ic4_sdk_candidates
    "C:/Program Files/The Imaging Source Europe GmbH/IC Imaging Control 4")
list(REMOVE_DUPLICATES _dual_ic4_sdk_candidates)

set(_dual_ic4_resolved_sdk_root "")
foreach(candidate IN LISTS _dual_ic4_sdk_candidates)
    dual_ic4_varjo_normalize_ic4_root("${candidate}" normalized_candidate)
    if(normalized_candidate)
        set(_dual_ic4_resolved_sdk_root "${normalized_candidate}")
        break()
    endif()
endforeach()

if(NOT _dual_ic4_resolved_sdk_root)
    string(JOIN "\n  - " _dual_ic4_candidate_text ${_dual_ic4_sdk_candidates})
    message(FATAL_ERROR
        "IC Imaging Control 4 SDK was not found.\n"
        "Expected: <root>/include/ic4/ic4.h and <root>/lib/x64/ic4core.lib\n"
        "Checked candidates:\n  - ${_dual_ic4_candidate_text}\n"
        "Configure again with, for example:\n"
        "  -DIC4_SDK_ROOT=\"C:/Users/<name>/AppData/Local/Programs/The Imaging Source Europe GmbH/IC Imaging Control 4\"")
endif()

if(NOT EXISTS "${_dual_ic4_resolved_sdk_root}/lib/x64/ic4core.lib")
    message(FATAL_ERROR
        "IC4 headers were found, but ic4core.lib is missing: "
        "${_dual_ic4_resolved_sdk_root}/lib/x64/ic4core.lib")
endif()

set(IC4_SDK_ROOT "${_dual_ic4_resolved_sdk_root}" CACHE PATH "Path to the IC Imaging Control 4 SDK root" FORCE)
message(STATUS "DualIC4VarjoApp: IC4 SDK root = ${IC4_SDK_ROOT}")
message(STATUS "DualIC4VarjoApp: IC4 header   = ${IC4_SDK_ROOT}/include/ic4/ic4.h")
message(STATUS "DualIC4VarjoApp: IC4 library  = ${IC4_SDK_ROOT}/lib/x64/ic4core.lib")

# VarjoXR v0.1.0 normally pins an older D3D12Helper commit. Override it so
# VarjoXR and IC4Ext v1.0.1 share the same D3D12Helper target and ABI.
set(VARJOXR_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
set(VARJOXR_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
set(VARJOXR_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VARJOXR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VARJOXR_D3D12HELPER_GIT_TAG "${DUAL_IC4_VARJO_D3D12HELPER_TAG}" CACHE STRING "" FORCE)
set(D3D12HELPER_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_INSTALL OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_ENABLE_PACKAGE_SMOKE_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    VarjoXR
    GIT_REPOSITORY https://github.com/Isasa2357/VarjoXR.git
    GIT_TAG        ${DUAL_IC4_VARJO_VARJOXR_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(VarjoXR)

set(IC4EXT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(IC4EXT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IC4EXT_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
set(IC4EXT_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
set(IC4EXT_D3D12HELPER_GIT_TAG "${DUAL_IC4_VARJO_D3D12HELPER_TAG}" CACHE STRING "" FORCE)

FetchContent_Declare(
    IC4Ext
    GIT_REPOSITORY https://github.com/Isasa2357/IC4Ext.git
    GIT_TAG        ${DUAL_IC4_VARJO_IC4EXT_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(IC4Ext)

add_executable(DualIC4VarjoApp
    src/AppConfig.cpp
    src/AppConfig.hpp
    src/RenderedFrameMetadataLogger.cpp
    src/RenderedFrameMetadataLogger.hpp
    src/StereoDisplayTextureRing.cpp
    src/StereoDisplayTextureRing.hpp
    src/TimeUtil.cpp
    src/TimeUtil.hpp
    src/main.cpp)

target_compile_features(DualIC4VarjoApp PRIVATE cxx_std_17)
target_compile_definitions(DualIC4VarjoApp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DualIC4VarjoApp PRIVATE
    IC4Ext::IC4Ext
    VarjoXR::VarjoXR)

if(MSVC)
    target_compile_options(DualIC4VarjoApp PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

# VarjoXR exposes these helper functions from its CMake modules.
if(COMMAND varjoxr_copy_dxc_runtime)
    varjoxr_copy_dxc_runtime(DualIC4VarjoApp)
endif()
if(COMMAND varjoxr_copy_varjo_runtime)
    varjoxr_copy_varjo_runtime(DualIC4VarjoApp)
endif()

# IC4 SDK itself is installed locally and is not fetched by IC4Ext.
find_file(DUAL_IC4_VARJO_IC4CORE_DLL
    NAMES ic4core.dll
    PATHS
        "${IC4_SDK_ROOT}/bin/x64"
        "${IC4_SDK_ROOT}/bin"
    NO_DEFAULT_PATH)
if(DUAL_IC4_VARJO_IC4CORE_DLL)
    add_custom_command(TARGET DualIC4VarjoApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DUAL_IC4_VARJO_IC4CORE_DLL}"
                "$<TARGET_FILE_DIR:DualIC4VarjoApp>"
        VERBATIM)
else()
    message(WARNING "ic4core.dll was not found under IC4_SDK_ROOT. Ensure the IC4 SDK bin/x64 directory is on PATH.")
endif()
