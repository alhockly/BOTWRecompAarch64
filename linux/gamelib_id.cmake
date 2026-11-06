# GAMELIB_ID for the Linux build. Same inputs as android/gamelib_id.cmake except the
# generator flag is "--simd asm" with no --dyn-base (fixed guest window).
# Needs R (the repository). Sets GAMELIB_ID and GAMELIB_FLAGS.
set(GAMELIB_FLAGS -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC)
set(_gamelib_srcs tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py
    tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt
    runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h)
set(_id_text "")
foreach(f ${_gamelib_srcs})
    file(READ ${R}/${f} _t)
    string(APPEND _id_text "${_t}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${R}/${f})
endforeach()
string(SHA256 _id "${_id_text}${GAMELIB_FLAGS} --simd asm")
string(SUBSTRING ${_id} 0 16 GAMELIB_ID)
set(GAMELIB_CFLAGS_STR "-O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC")
set(GAMELIB_SHARED_STR "-shared")
