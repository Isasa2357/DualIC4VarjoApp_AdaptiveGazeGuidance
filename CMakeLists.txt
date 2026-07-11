cmake_minimum_required(VERSION 3.21)

project(DualIC4VarjoApp_AdaptiveGazeGuidance
    VERSION 0.9.0
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "This application requires Windows, Direct3D 12, IC Imaging Control 4, and Varjo Runtime.")
endif()

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Do not update populated dependencies automatically" FORCE)

set(DUAL_IC4_VARJO_IC4EXT_TAG
    "50db99c3e18a08af683ef69352cc52ffb55a7a2d")
set(DUAL_IC4_VARJO_VARJOXR_TAG
    "3d28a950b54edeafa3e7b595b78cc3b47cdc2ef5")
set(DUAL_IC4_VARJO_VARJOTOOLKIT_TAG
    "8512ba418dc0e63c67b6e0518ef84dc2b53fb39d")
set(DUAL_IC4_VARJO_D3D12HELPER_TAG "v1.13.0")
set(DUAL_IC4_VARJO_IMGUI_TAG "v1.92.5")

# -----------------------------------------------------------------------------
# IC Imaging Control 4 SDK discovery
# -----------------------------------------------------------------------------
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
        "Configure with -DIC4_SDK_ROOT=<IC Imaging Control 4 root>.")
endif()

if(NOT EXISTS "${_dual_ic4_resolved_sdk_root}/lib/x64/ic4core.lib")
    message(FATAL_ERROR
        "IC4 headers were found, but ic4core.lib is missing: "
        "${_dual_ic4_resolved_sdk_root}/lib/x64/ic4core.lib")
endif()

set(IC4_SDK_ROOT "${_dual_ic4_resolved_sdk_root}"
    CACHE PATH "Path to the IC Imaging Control 4 SDK root" FORCE)
message(STATUS "DualIC4VarjoApp: IC4 SDK root = ${IC4_SDK_ROOT}")

# -----------------------------------------------------------------------------
# VarjoToolkit / VarjoXR / D3D12Helper / IC4Ext
# -----------------------------------------------------------------------------
if(VARJOXR_VARJO_SDK_ROOT)
    set(VARJO_SDK_ROOT "${VARJOXR_VARJO_SDK_ROOT}"
        CACHE PATH "Path to Varjo Native SDK" FORCE)
endif()

set(VARJOTOOLKIT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VARJOTOOLKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VARJOTOOLKIT_BUILD_HMD_TESTS OFF CACHE BOOL "" FORCE)
set(VARJOTOOLKIT_ENABLE_EXPERIMENTAL_MR_POSTPROCESS OFF CACHE BOOL "" FORCE)
if(NOT TARGET VarjoToolkit::VarjoToolkit)
    FetchContent_Declare(
        VarjoToolkit
        GIT_REPOSITORY https://github.com/Isasa2357/VarjoToolkit.git
        GIT_TAG        ${DUAL_IC4_VARJO_VARJOTOOLKIT_TAG}
        GIT_SHALLOW    FALSE)
    FetchContent_MakeAvailable(VarjoToolkit)
endif()

set(VARJOXR_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
set(VARJOXR_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
set(VARJOXR_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VARJOXR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VARJOXR_VARJOTOOLKIT_GIT_TAG
    "${DUAL_IC4_VARJO_VARJOTOOLKIT_TAG}" CACHE STRING "" FORCE)
set(VARJOXR_D3D12HELPER_GIT_TAG
    "${DUAL_IC4_VARJO_D3D12HELPER_TAG}" CACHE STRING "" FORCE)
set(D3D12HELPER_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_INSTALL OFF CACHE BOOL "" FORCE)
set(D3D12HELPER_ENABLE_PACKAGE_SMOKE_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    VarjoXR
    GIT_REPOSITORY https://github.com/Isasa2357/VarjoXR.git
    GIT_TAG        ${DUAL_IC4_VARJO_VARJOXR_TAG}
    GIT_SHALLOW    FALSE)
FetchContent_MakeAvailable(VarjoXR)

set(IC4EXT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(IC4EXT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IC4EXT_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
set(IC4EXT_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
set(IC4EXT_D3D12HELPER_GIT_TAG
    "${DUAL_IC4_VARJO_D3D12HELPER_TAG}" CACHE STRING "" FORCE)

FetchContent_Declare(
    IC4Ext
    GIT_REPOSITORY https://github.com/Isasa2357/IC4Ext.git
    GIT_TAG        ${DUAL_IC4_VARJO_IC4EXT_TAG}
    GIT_SHALLOW    FALSE)
FetchContent_MakeAvailable(IC4Ext)

# -----------------------------------------------------------------------------
# Dear ImGui Win32 + DirectX 12 backends
# -----------------------------------------------------------------------------
FetchContent_Declare(
    DearImGui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${DUAL_IC4_VARJO_IMGUI_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_GetProperties(DearImGui)
if(NOT dearimgui_POPULATED)
    FetchContent_Populate(DearImGui)
endif()

add_library(DualIC4VarjoImGui STATIC
    "${dearimgui_SOURCE_DIR}/imgui.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_draw.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_tables.cpp"
    "${dearimgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${dearimgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${dearimgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
add_library(DualIC4Varjo::ImGui ALIAS DualIC4VarjoImGui)

target_compile_features(DualIC4VarjoImGui PUBLIC cxx_std_17)
target_include_directories(DualIC4VarjoImGui
    PUBLIC
        "${dearimgui_SOURCE_DIR}"
        "${dearimgui_SOURCE_DIR}/backends")
target_compile_definitions(DualIC4VarjoImGui
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
target_link_libraries(DualIC4VarjoImGui
    PUBLIC
        d3d12
        dxgi
        dxguid
        imm32)
if(MSVC)
    target_compile_options(DualIC4VarjoImGui PRIVATE
        /W4 /permissive- /utf-8 /EHsc)
endif()

# -----------------------------------------------------------------------------
# Application
# -----------------------------------------------------------------------------
add_executable(DualIC4VarjoApp
    src/AppConfig.cpp
    src/AppConfig.hpp
    src/ApplicationMain.cpp
    src/ExperimentOutput.cpp
    src/ExperimentOutput.hpp
    src/ImGuiStereoPreview.cpp
    src/ImGuiStereoPreview.hpp
    src/RenderedFrameMetadataLogger.cpp
    src/RenderedFrameMetadataLogger.hpp
    src/StereoCalibrationSupport.cpp
    src/StereoCalibrationSupport.hpp
    src/StereoDisplayTextureRing.cpp
    src/StereoDisplayTextureRing.hpp
    src/TimeUtil.cpp
    src/TimeUtil.hpp)

target_compile_features(DualIC4VarjoApp PRIVATE cxx_std_17)
target_compile_definitions(DualIC4VarjoApp PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
target_link_libraries(DualIC4VarjoApp PRIVATE
    IC4Ext::IC4Ext
    VarjoXR::VarjoXR
    VarjoToolkit::VarjoToolkit
    DualIC4Varjo::ImGui
    nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(DualIC4VarjoApp PRIVATE
        /W4 /permissive- /utf-8 /EHsc)
endif()

if(COMMAND varjoxr_copy_dxc_runtime)
    varjoxr_copy_dxc_runtime(DualIC4VarjoApp)
endif()
if(COMMAND varjoxr_copy_varjo_runtime)
    varjoxr_copy_varjo_runtime(DualIC4VarjoApp)
endif()

find_file(DUAL_IC4_VARJO_IC4CORE_DLL
    NAMES ic4core.dll
    PATHS "${IC4_SDK_ROOT}/bin/x64" "${IC4_SDK_ROOT}/bin"
    NO_DEFAULT_PATH)
if(DUAL_IC4_VARJO_IC4CORE_DLL)
    add_custom_command(TARGET DualIC4VarjoApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DUAL_IC4_VARJO_IC4CORE_DLL}"
                "$<TARGET_FILE_DIR:DualIC4VarjoApp>"
        VERBATIM)
else()
    message(WARNING
        "ic4core.dll was not found under IC4_SDK_ROOT. "
        "Ensure its bin/x64 directory is on PATH.")
endif()
