#!/usr/bin/env bash
# ps5-spotify - Builds the static libraries the app links: mbedTLS and bell's
# network layer, with the app's PS5 compiler wrapper.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Output: build/deps/libmbedtls.a, build/deps/libbell.a
# ponytail: rebuilds everything each run (~1 min); move into the ninja graph if
# that starts to hurt.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root="$root/.deps/native/ps5-payload-sdk"
bell="$root/../cspot/cspot/bell"
out="$root/build/deps"

mbedtls_version=3.6.4
mbedtls_sha256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
mbedtls_tar="$root/.deps/src/mbedtls-$mbedtls_version.tar.bz2"
mbedtls="$root/.deps/src/mbedtls-$mbedtls_version"

mkdir -p "$root/.deps/src" "$out"
if [[ ! -f $mbedtls_tar ]]; then
    curl -sSL -o "$mbedtls_tar" \
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2"
fi
echo "$mbedtls_sha256  $mbedtls_tar" | sha256sum --check --quiet
[[ -d $mbedtls ]] || tar -xjf "$mbedtls_tar" -C "$root/.deps/src"

cc() {
    env PS5_PAYLOAD_SDK="$sdk_root" PS5_CLANG="${PS5_CLANG:-clang}" USE_CCACHE=0 \
        sh "$root/tooling/prospero-clang18" "$@"
}
export -f cc
export root sdk_root

mbedtls_flags=(-std=c11 -O2 -w -ffunction-sections -fdata-sections
    -D__unix__ "-DMBEDTLS_USER_CONFIG_FILE=\"$root/src/platform/mbedtls_ps5_config.h\""
    -I"$mbedtls/include")
bell_flags=(-std=c++17 -O2 -w -fexceptions -frtti -ffunction-sections -fdata-sections
    -DBELL_DISABLE_FMT -DBELL_DISABLE_REGEX
    "-DMBEDTLS_USER_CONFIG_FILE=\"$root/src/platform/mbedtls_ps5_config.h\""
    -I"$mbedtls/include" -I"$bell/main/io/include" -I"$bell/main/utilities/include"
    -I"$bell/main/platform")

# Compiles $3.. into objects under $1 with the flags in array named $2.
compile() {
    local dir=$1 flags_name=$2
    shift 2
    local -n flags=$flags_name
    rm -rf "$dir" && mkdir -p "$dir"
    local pids=() status=0
    for source in "$@"; do
        cc "${flags[@]}" -c "$source" -o "$dir/$(basename "$source").o" &
        pids+=($!)
        if (( ${#pids[@]} >= $(nproc) )); then
            wait "${pids[0]}" || status=1
            pids=("${pids[@]:1}")
        fi
    done
    for pid in "${pids[@]}"; do wait "$pid" || status=1; done
    return $status
}

compile "$out/mbedtls" mbedtls_flags "$mbedtls"/library/*.c
rm -f "$out/libmbedtls.a" && llvm-ar rcs "$out/libmbedtls.a" "$out"/mbedtls/*.o

bell_sources=(
    "$bell/main/io/TLSSocket.cpp"
    "$bell/main/io/SocketStream.cpp"
    "$bell/main/io/URLParser.cpp"
    "$bell/main/io/HTTPClient.cpp"
    "$bell/main/io/X509Bundle.cpp"
    "$bell/main/utilities/BellLogger.cpp"
    "$bell/main/utilities/BellUtils.cpp"
)
compile "$out/bell" bell_flags "${bell_sources[@]}"
bell_c_flags=(-std=c11 -O2 -w -ffunction-sections -fdata-sections -I"$bell/main/io/include")
compile "$out/bell-c" bell_c_flags "$bell/main/io/picohttpparser.c"
rm -f "$out/libbell.a" && llvm-ar rcs "$out/libbell.a" "$out"/bell/*.o "$out"/bell-c/*.o
echo "deps: $out/libmbedtls.a $out/libbell.a"
