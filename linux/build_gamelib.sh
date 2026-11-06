#!/bin/bash
# linux/build_gamelib.sh <exefs folder>: botwgame.so for Linux aarch64.
# gen.py --simd asm, no --dyn-base (fixed guest window). Output: build/aarch64/botwgame.so and botwgame.so.id
set -euo pipefail
cd "$(dirname "$0")/.."
R=$PWD
EXEFS=${1:?usage: linux/build_gamelib.sh <exefs folder>}
GEN=${GAMELIB_GEN:-$R/generated-linux-aarch64}
OUT=${GAMELIB_OUT:-$R/build/aarch64}
W=$OUT/gamelib-build
PY=$R/tools/venv/bin/python
# Generated code uses clang's musttail attribute. GCC cannot compile it.
if [ "$(uname -m)" = "aarch64" ]; then
    TC=(-DCMAKE_C_COMPILER=clang)
else
    TC=(-DCMAKE_TOOLCHAIN_FILE="$R/linux/toolchain-aarch64-clang.cmake")
fi

if [ ! -x "$PY" ]; then
    python3 -m venv "$R/tools/venv"
    "$PY" -m pip install -q lz4 capstone unicorn
fi
if [ ! -f "$GEN/main/main_table.c" ] || [ ! -f "$GEN/sdk/sdk_table.c" ]; then
    (cd "$R/tools" && "$PY" gen.py sdk "$EXEFS/sdk" 0x7004000000 "$GEN/sdk" --hle ../runtime/hle/hle_symbols.txt --simd asm)
    (cd "$R/tools" && "$PY" gen.py main "$EXEFS/main" 0x7000000000 "$GEN/main" --simd asm)
fi
(cd "$GEN" && for f in main/*.c sdk/*.c; do
    awk -v f="$f" '/^[a-zA-Z].* f_[0-9a-f]+\(.*\) \{$/ {n = 0} /^L_[0-9a-f]+:/ {if (++n > 15000) {print f; exit}}' "$f"
done) > "$GEN/heavy.txt"
echo "heavy (at -O0): $(tr '\n' ' ' < "$GEN/heavy.txt")"
cmake -S "$R/linux/gamelib" -B "$W" -G Ninja "${TC[@]}" -DCMAKE_BUILD_TYPE=Release -DLAS_GEN="$GEN"
cmake --build "$W" -j"$(nproc 2>/dev/null || echo 8)"
mkdir -p "$OUT"
cp "$W/botwgame.so" "$W/botwgame.so.id" "$OUT/"
ls -l "$OUT/botwgame.so" "$OUT/botwgame.so.id"
