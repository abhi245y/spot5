#!/usr/bin/env bash
# spot5 - Renders the app's screens on the PC (Mesa, no console, no GPU).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/preview.sh [output dir]   (default build/preview)
# Builds src/screens + the UI kit for Linux with host/preview_main.cpp and
# writes one PNG per scripted moment. Look at them before deploying.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/tools/ninja-build.sh"
cxx=$(command -v "${HOST_CXX:-clang++}")
cc=$(command -v "${HOST_CC:-clang}")
export USE_CCACHE=${USE_CCACHE:-0}
build="$root/build/preview"
compiler_cache=()
ninja_begin "$build/build.ninja"

# The UI only: the kit and the screens. Console code (cspot, audio output,
# platform services, the app) is not part of the preview.
sources=("$root/host/preview_main.cpp" "$root/host/platform_host.cpp" "$root/src/service/home.cpp" "$root/src/service/library.cpp")
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$root/src/gfx" "$root/src/ui" "$root/src/core" "$root/src/screens" \
    "$root/src/audio" "$root/src/third_party/stb" -type f \( -name '*.cpp' -o -name '*.c' \) \
    ! -name player.cpp -print0 | sort -z)

objects=()
for source in "${sources[@]}"; do
    relative=${source#"$root/"}
    object="$build/obj/${relative//\//_}.o"
    if [[ $source == *.c ]]; then
        ninja_inputs=("$source" "$cc")
        ninja_edge CC "$object" "$cc" -std=c11 -O2 -w -I"$root/src" \
            -MD -MF "$object.d" -c "$source" -o "$object"
    else
        ninja_inputs=("$source" "$cxx")
        ninja_edge CXX "$object" "$cxx" -std=c++20 -O2 -Wall -Wextra \
            -DGL_GLEXT_PROTOTYPES=1 -I"$root/src" -isystem "$root/../cspot/cspot/bell/external/nlohmann_json/include" \
            -MD -MF "$object.d" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
ninja_inputs=("${objects[@]}")
ninja_edge LINK "$build/spt_preview" "$cxx" "${objects[@]}" -lEGL -lGL -lm -o "$build/spt_preview"
if ! ninja_run >"$build/build.log" 2>&1; then
    grep -E 'error|FAILED' -A6 "$build/build.log" | head -80 >&2
    exit 1
fi
grep -E 'src/screens.*warning|host/.*warning' -A4 "$build/build.log" | head -40 >&2 || true

output=${1:-"$root/build/preview"}
mkdir -p "$output"
run_preview() {
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
        "$build/spt_preview" "$root/assets" "$output"
}
run_preview
# Home covers the fixture lacks: fetch them (public CDN), then render again.
missing="$root/tests/home-fixture/missing-covers.txt"
if [[ -s $missing ]]; then
    echo "fetching $(wc -l < "$missing") Home covers for the preview..."
    while read -r url file; do
        curl -sfL -m 20 -o "$file" "$url" || rm -f "$file"
        # The PC preview decodes with stb only: WebP covers become PNG here.
        [[ -f $file ]] && python3 -c '
import sys
from PIL import Image
path = sys.argv[1]
head = open(path, "rb").read(12)
if head[:4] == b"RIFF" and head[8:12] == b"WEBP":
    Image.open(path).save(path, "PNG")' "$file" || rm -f "$file"
        sleep 0.2
    done < "$missing"
    run_preview
fi
