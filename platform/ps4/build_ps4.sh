#!/usr/bin/env bash
# build_ps4.sh — build the DKR native port for the PlayStation 4 (Piglet/GLES2).
#
# The engine is built unchanged; only the SDL surface is replaced, by the
# subset in platform/ps4 (window/GL via EGL+Piglet, input via libScePad, audio
# via libSceAudioOut, pthread for threads).
#
# Usage: ./platform/ps4/build_ps4.sh [--clean]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build-ps4"
OO="${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN to your OpenOrbis PS4Toolchain directory}"
CC="${PS4_CC:-clang++}"
LD="${PS4_LD:-ld.lld}"
JOBS="${JOBS:-$(nproc)}"

# Auth block: the exact one a working port on FW 12.02 uses. Without it the
# eboot renders into its back buffer and never gets a canvas (0x80aa000b).
AUTH_INFO="000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
PAID="0x3800000000000035"

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$OUT"
fi
mkdir -p "$OUT/obj"

CFLAGS=(
    -cc1 -x c -triple x86_64-scei-ps4-elf -std=c11 ${PS4_OPT:--O2}
    -ffreestanding -nostdinc++ -nobuiltininc -fno-builtin -nostdsysteminc
    -Wno-incompatible-function-pointer-types -Wno-int-conversion
    -Wno-int-to-void-pointer-cast -Wno-void-pointer-to-int-cast
    -Wno-implicit-function-declaration -Wno-parentheses-equality
    -Wno-unused-value -Wno-format -Wno-tautological-compare
    # Project headers FIRST: the toolchain ships its own memory.h/string.h, and
    # with the Orbis include dir in front, "memory.h" resolved to the system one
    # instead of the engine's type definitions.
    -I"$ROOT/platform/ps4"
    -I"$ROOT/game" -I"$ROOT/game/src" -I"$ROOT/game/include"
    -iquote"$ROOT/game/include/PR" -iquote"$ROOT/game/include/sys"
    -I"$OO/include" -I"$OO/include/orbis/_types"
    -I"$ROOT/game/libultra" -I"$ROOT/game/libultra/src/audio"
    -I"$ROOT/platform" -I"$ROOT/platform/fast3d" -I"$ROOT/platform/fast3d_shim"
    -I"$ROOT/lib/glad/include" -I"$ROOT/lib/stb" -I"$ROOT/lib/miniz" -I"$ROOT/lib/dr_libs"
    -DAVOID_UB=1 -DF3DDKR_GBI -DMDKR_APP -DMDKR_VERSION_STRING='"1.8.0"'
    -DMODERN_CC -DNATIVE_PORT=1 -DNON_MATCHING=1 -DSDL_MAIN_HANDLED
    -DVERSION_us_v80 -D_FINALROM -D_LANGUAGE_C -DNDEBUG
    -DPLATFORM_PS4=1
    # sigaction/siginfo behind the feature-test macros in the Orbis headers
    -D_GNU_SOURCE -D_BSD_SOURCE
    -DSTBI_NO_SIMD -DSTBI_NO_STDIO_IF_UNUSED=1
    -include ps4_time_compat.h
)

INCLUDES=(
    -I"$ROOT/build-gl/_deps/mdkr_harfbuzz-src/src"
    -I"$ROOT/build-gl/_deps/mdkr_sheenbidi-src/Headers"
)

# ---- source list: the mdkr64 target as the desktop build sees it -----------
SRC_LIST="$OUT/sources.txt"
if [[ ! -f "$SRC_LIST" ]]; then
    python3 - "$ROOT" > "$SRC_LIST" <<'PY'
import json, sys, pathlib
root = pathlib.Path(sys.argv[1])
cc = root / "build-gl" / "compile_commands.json"
data = json.loads(cc.read_text())
seen = []
for e in data:
    if 'mdkr64.dir' not in (e.get('output') or ''):
        continue
    f = e['file']
    # The native WebGPU backend (on by default on the desktop) has no PS4
    # equivalent; gfx_webgpu_lifecycle.c is backend-neutral and stays.
    name = pathlib.Path(f).name
    if (name.startswith('gfx_webgpu') and name != 'gfx_webgpu_lifecycle.c'):
        continue
    if f not in seen:
        seen.append(f)
for f in sorted(seen):
    print(f)
PY
fi

build_one() {
    local src="$1"
    local obj="$OUT/obj/$(echo "${src#$ROOT/}" | tr '/' '_').o"
    if [[ -f "$obj" && "$obj" -nt "$src" ]]; then
        return 0
    fi
    if ! "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" "-DMDKR_BUILD_STAMP=\"${APP_VER:-dev}\"" -emit-obj -o "$obj" "$src" 2> "$obj.log"; then
        echo "FAIL $src"
        return 1
    fi
    return 0
}
export -f build_one
export CC OUT ROOT
export CFLAGS_STR="${CFLAGS[*]}"
export INCLUDES_STR="${INCLUDES[*]}"
# The stamp is appended AFTER -cc1 below, because a -D in front of clang's -cc1
# is dropped. It must not be a header: a generated header would invalidate all
# 255 objects on every version bump and turn a one-line change into a full
# rebuild.
export STAMP_DEF="-DMDKR_BUILD_STAMP=\"${APP_VER:-dev}\""

# Re-export through a wrapper so xargs can call the function with the arrays.
cat > "$OUT/compile_one.sh" <<'EOS'
#!/usr/bin/env bash
set -euo pipefail
src="$1"
obj="$OUT/obj/$(echo "${src#$ROOT/}" | tr '/' '_').o"
if [[ -f "$obj" && "$obj" -nt "$src" ]]; then exit 0; fi
# shellcheck disable=SC2086
if ! $CC $CFLAGS_STR $INCLUDES_STR -emit-obj -o "$obj" "$src" "$STAMP_DEF" 2> "$obj.log"; then
  echo "FAIL $src"
  exit 1
fi
EOS
chmod +x "$OUT/compile_one.sh"

# Objects compiled before a header changed are still "newer than their source".
# Drop any object whose previous log recorded an error so it is rebuilt.
for log in "$OUT"/obj/*.log; do
    [[ -f "$log" ]] || continue
    if grep -qE "error:|undeclared function 'kill'" "$log" 2>/dev/null; then
        rm -f "${log%.log}"
    fi
done

echo "[ps4] sources: $(wc -l < "$SRC_LIST")"
if ! xargs -P "$JOBS" -n 1 "$OUT/compile_one.sh" < "$SRC_LIST"; then
    echo "[ps4] compile failures:"
    grep -l . "$OUT"/obj/*.log 2>/dev/null | head -5
    exit 1
fi

# Shader transform is done at runtime by the GL backend, so no generated code.
# Extra TUs that are PS4-only replacements.
PS4_TUS=("$ROOT/platform/ps4/sdl_ps4.c" "$ROOT/platform/ps4/ps4_text_stubs.c")
# main_pc.c provides main(); rename it so ps4_entry.c owns the entry point.
RENAMED_MAIN="$OUT/obj/platform_main_pc.c.o"
PS4_TUS+=("$ROOT/platform/ps4/ps4_entry.c")

for src in "${PS4_TUS[@]}"; do
    obj="$OUT/obj/$(echo "${src#$ROOT/}" | tr '/' '_').o"
    "$CC" "${CFLAGS[@]}" "${INCLUDES[@]}" "-DMDKR_BUILD_STAMP=\"${APP_VER:-dev}\"" -emit-obj -o "$obj" "$src" 2> "$obj.log" || {
        echo "FAIL $src"; head -20 "$obj.log"; exit 1;
    }
done

# platform_main_pc.c.o comes from the source list; drop any stale renamed copy.
rm -f "$OUT/obj/main_pc_renamed.o"

echo "[ps4] linking"
OBJ_LIST=("$RENAMED_MAIN")
while IFS= read -r src; do
    if [[ "$src" == *"/platform/main_pc.c" ]]; then continue; fi
    OBJ_LIST+=("$OUT/obj/$(echo "${src#$ROOT/}" | tr '/' '_').o")
done < "$SRC_LIST"
for src in "${PS4_TUS[@]}"; do
    OBJ_LIST+=("$OUT/obj/$(echo "${src#$ROOT/}" | tr '/' '_').o")
done

# Graphics backend: Piglet (Sony).
LINK_GRAPHICS=(-lScePigletv2VSH)

# Image data bounds for stubs_dkr.c ("LOW-IMAGE HOSTS"): eboot.bin loads below
# 4 GB, so game globals must be told apart from 32-bit tokens by address range.
# ld.lld with this linker script defines neither _etext nor _end.
IMAGE_SYMS=(--defsym=mdkr_image_data_start=ADDR\(.rodata\)
            "--defsym=mdkr_image_data_end=ADDR(.bss)+SIZEOF(.bss)")
"$LD" -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr \
    "${IMAGE_SYMS[@]}" \
    "$OO/lib/crt1.o" "${OBJ_LIST[@]}" -o "$OUT/mdkr64.elf" \
    -L"$OO/lib" -lkernel -lSceLibcInternal -lScePosix -lc \
    -lSceSysmodule -lSceSystemService -lSceUserService -lSceAudioOut -lScePad \
    "${LINK_GRAPHICS[@]}" -lm 2> "$OUT/link.log" || {
    echo "[ps4] link failed:"; head -30 "$OUT/link.log"; exit 1;
}
echo "[ps4] elf: $(stat -c%s "$OUT/mdkr64.elf") bytes"

export OO_PS4_TOOLCHAIN="$OO"
"$OO/bin/linux/create-fself" -in="$OUT/mdkr64.elf" -out="$OUT/mdkr64.oelf" \
    --eboot="$OUT/eboot.bin" --paid="$PAID" --authinfo="$AUTH_INFO"
echo "[ps4] eboot.bin: $(stat -c%s "$OUT/eboot.bin") bytes"
