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
if(IC4_SDK_ROOT)
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
endif()
