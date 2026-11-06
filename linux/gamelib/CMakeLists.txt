# botwgame.so for Linux aarch64. Generated with gen.py --simd asm and no --dyn-base.
# Runtime symbols stay undefined; the -rdynamic botwrecomp executable provides them at dlopen.
#   LAS_GEN: generated sdk/ and main/
cmake_minimum_required(VERSION 3.22)
project(botwgame C)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(R ${R} ABSOLUTE)
include(${R}/linux/gamelib_id.cmake)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

file(GLOB GEN_SRC ${LAS_GEN}/main/*.c ${LAS_GEN}/sdk/*.c)
list(LENGTH GEN_SRC n)
if(n EQUAL 0)
    message(FATAL_ERROR "no generated code in ${LAS_GEN}")
endif()

add_library(botwgame SHARED ${GEN_SRC} ${R}/tools/gamelib_entry.c)
target_include_directories(botwgame PRIVATE ${R}/runtime)
target_compile_options(botwgame PRIVATE ${GAMELIB_FLAGS})
set_source_files_properties(${R}/tools/gamelib_entry.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
if(EXISTS ${LAS_GEN}/heavy.txt)
    file(STRINGS ${LAS_GEN}/heavy.txt HEAVY)
    foreach(f ${HEAVY})
        if(NOT f STREQUAL "")
            set_source_files_properties(${LAS_GEN}/${f} PROPERTIES COMPILE_OPTIONS "-O0")
        endif()
    endforeach()
endif()
set_target_properties(botwgame PROPERTIES PREFIX "")
file(WRITE ${CMAKE_BINARY_DIR}/botwgame.so.id "${GAMELIB_ID}\n")
