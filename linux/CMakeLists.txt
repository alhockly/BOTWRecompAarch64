# botwrecomp for Linux. Android stays on the Gradle/NDK path.
# Native aarch64:  cmake -S linux -B build/aarch64 -DCMAKE_BUILD_TYPE=Release -DDEPS=$PWD/linux/aarch64
# Cross from x86-64: add -DCMAKE_TOOLCHAIN_FILE=$PWD/linux/toolchain-aarch64.cmake
cmake_minimum_required(VERSION 3.22)
project(botwrecomp C CXX)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(R ${R} ABSOLUTE)
if(NOT DEPS)
    set(DEPS ${R}/linux/aarch64)
endif()
list(PREPEND CMAKE_PREFIX_PATH ${DEPS})

if(NOT EXISTS ${DEPS}/lib/libSDL3.so)
    message(FATAL_ERROR "SDL3 missing in ${DEPS}. Run linux/build_deps.sh first.")
endif()

include(${R}/linux/gamelib_id.cmake)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

file(GLOB RT_SRC CONFIGURE_DEPENDS
    ${R}/runtime/*.c ${R}/runtime/hle/*.c ${R}/runtime/services/*.c
    ${R}/runtime/nvn/*.c ${R}/runtime/nvn/maxwell/*.c)
list(REMOVE_ITEM RT_SRC ${R}/runtime/hle/movie.c)
file(GLOB VK_SRC CONFIGURE_DEPENDS ${R}/runtime/nvn/vulkan/*.c ${R}/runtime/nvn/vulkan/*.cpp)
list(REMOVE_ITEM VK_SRC ${R}/runtime/nvn/vulkan/android_driver.c)
file(GLOB UI_SRC CONFIGURE_DEPENDS ${R}/runtime/ui/*.mm)
set(IMGUI ${R}/third_party/imgui)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)

set(SMAA_INC ${CMAKE_BINARY_DIR}/inc/smaa_hlsl.inc)
add_custom_command(
    OUTPUT ${SMAA_INC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/inc
    COMMAND python3 ${R}/linux/gen_smaa.py ${R}/third_party/smaa/SMAA.hlsl ${SMAA_INC}
    DEPENDS ${R}/third_party/smaa/SMAA.hlsl ${R}/linux/gen_smaa.py
    VERBATIM)

set(SHADERC_LIB ${DEPS}/lib/libshaderc_shared.so)
set(VULKAN_LIB ${DEPS}/lib/libvulkan.so)
if(NOT EXISTS ${SHADERC_LIB})
    message(FATAL_ERROR "shaderc missing in ${DEPS}. Run linux/build_deps.sh first.")
endif()
if(NOT EXISTS ${VULKAN_LIB} AND NOT EXISTS ${DEPS}/lib/libvulkan.so.1)
    message(FATAL_ERROR "libvulkan missing in ${DEPS}. Run linux/build_deps.sh first.")
endif()
if(NOT EXISTS ${VULKAN_LIB})
    set(VULKAN_LIB ${DEPS}/lib/libvulkan.so.1)
endif()
set(HAVE_FFMPEG FALSE)
if(EXISTS ${DEPS}/lib/libavformat.so)
    set(HAVE_FFMPEG TRUE)
    list(APPEND RT_SRC ${R}/runtime/hle/movie.c)
endif()

add_executable(botwrecomp ${RT_SRC} ${R}/runtime/platform/linux.c ${VK_SRC} ${UI_SRC} ${IMGUI_SRC} ${SMAA_INC})
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")
set_source_files_properties(${R}/runtime/gamelib.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
set_source_files_properties(${R}/runtime/gamebuild.c PROPERTIES COMPILE_DEFINITIONS
    "GAMELIB_ID=\"${GAMELIB_ID}\";GAMELIB_CFLAGS=\"${GAMELIB_CFLAGS_STR}\";GAMELIB_SHARED=\"${GAMELIB_SHARED_STR}\"")

target_include_directories(botwrecomp PRIVATE
    ${R}/runtime ${R}/runtime/nvn ${IMGUI} ${IMGUI}/backends
    ${DEPS}/include ${CMAKE_BINARY_DIR}/inc ${R}/third_party/smaa/Textures)
target_compile_options(botwrecomp PRIVATE -O2 -g -fno-strict-aliasing -fPIC -Wall -Wno-unused-function -Wno-format
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
if(NOT HAVE_FFMPEG)
    target_compile_definitions(botwrecomp PRIVATE LARECOMP_NO_FFMPEG)
endif()
target_link_libraries(botwrecomp PRIVATE
    ${DEPS}/lib/libSDL3.so ${SHADERC_LIB} ${VULKAN_LIB} pthread dl m)
if(HAVE_FFMPEG)
    # The cross link only needs the SONAMEs. Fedora's libavcodec pulls in codec plugins
    # that are not in this sysroot; the handheld's own FFmpeg provides them at run time.
    target_link_libraries(botwrecomp PRIVATE
        ${DEPS}/lib/libavformat.so ${DEPS}/lib/libavcodec.so ${DEPS}/lib/libavutil.so ${DEPS}/lib/libswscale.so)
    target_link_options(botwrecomp PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_options(botwrecomp PRIVATE -rdynamic -Wl,-z,nostart-stop-gc -Wl,-rpath,${DEPS}/lib)
