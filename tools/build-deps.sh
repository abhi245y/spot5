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

# The app builds from whatever branch ../cspot has checked out. Refuse a
# checkout that lacks the fixes the console needs (a branch cut from master
# would silently drop them), and say what is being built.
grep -q "login5.spotify.com" "$root/../cspot/cspot/src/AccessKeyFetcher.cpp" || {
    echo "cspot checkout lacks login5 (feat/login5-token); check out the ps5/feature branch" >&2
    exit 2
}
grep -q "appendToContext" "$root/../cspot/cspot/src/SpircHandler.cpp" || {
    echo "cspot checkout lacks local playback (feat/local-load)" >&2
    exit 2
}
grep -q "statusCode" "$root/../cspot/cspot/bell/main/io/include/HTTPClient.h" || {
    echo "bell checkout lacks HTTPClient::statusCode (bell ps5 branch)" >&2
    exit 2
}
grep -q "hasAccept" "$root/../cspot/cspot/bell/main/io/HTTPClient.cpp" || {
    echo "bell checkout lacks the Accept header fix (fix/accept-header)" >&2
    exit 2
}
echo "cspot: $(git -C "$root/../cspot" rev-parse --abbrev-ref HEAD)@$(git -C "$root/../cspot" rev-parse --short HEAD)" \
    "bell: $(git -C "$root/../cspot/cspot/bell" rev-parse --short HEAD)"
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
cspot="$root/../cspot/cspot"
gen="$out/proto"
common_defs=(-DBELL_TASK_MIN_STACK=1048576 -DCSPOT_CDN_CHUNK_SIZE=262144 -DBELL_DISABLE_FMT -DBELL_DISABLE_REGEX -DPB_ENABLE_MALLOC -DPB_FIELD_32BIT
    "-DMBEDTLS_USER_CONFIG_FILE=\"$root/src/platform/mbedtls_ps5_config.h\"")
includes=(-I"$mbedtls/include" -I"$bell/main/io/include" -I"$bell/main/utilities/include"
    -I"$bell/main/platform" -I"$bell/external/nanopb" -I"$bell/external/nlohmann_json/include"
    -I"$cspot/include" -I"$gen" -I"$bell/external/tremor"
    -I"$bell/external/civetweb/include")
bell_flags=(-std=c++17 -O2 -w -fexceptions -frtti -ffunction-sections -fdata-sections
    "${common_defs[@]}" "${includes[@]}")
bell_c_flags=(-std=c11 -O2 -w -ffunction-sections -fdata-sections
    "${common_defs[@]}" "${includes[@]}")

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

# nanopb C files from cspot's .proto files (same generator as the PC build).
rm -rf "$gen" && mkdir -p "$gen/protobuf"
(cd "$cspot" && "$root/../cspot/.venv312/bin/python" "$bell/external/nanopb/generator/nanopb_generator.py" \
    -q -D "$gen" protobuf/*.proto)

bell_sources=(
    "$bell/main/io/TLSSocket.cpp"
    "$bell/main/io/SocketStream.cpp"
    "$bell/main/io/URLParser.cpp"
    "$bell/main/io/HTTPClient.cpp"
    "$bell/main/io/X509Bundle.cpp"
    "$bell/main/utilities/BellLogger.cpp"
    "$bell/main/utilities/BellUtils.cpp"
    "$bell/main/utilities/Crypto.cpp"
    "$bell/main/utilities/NanoPBHelper.cpp"
    "$bell/main/utilities/NanoPBExtensions.cpp"
    "$bell/main/platform/linux/WrappedSemaphore.cpp"
    # Zeroconf pairing: web server for the phone's login (mDNS is the helper
    # payload's job, helper/mdns_helper.c: apps may not bind its port).
    "$bell/main/io/BellHTTPServer.cpp"
    "$bell/external/civetweb/CivetServer.cpp"
)
for name in AccessKeyFetcher ApResolve AuthChallenges LoginBlob MercurySession \
    PlainConnection Session ShannonConnection Shannon TimeProvider Utils \
    TrackQueue TrackReference CDNAudioFile PlaybackState SpircHandler TrackPlayer; do
    bell_sources+=("$cspot/src/$name.cpp")
done
compile "$out/bell" bell_flags "${bell_sources[@]}"
compile "$out/bell-c" bell_c_flags "$bell/main/io/picohttpparser.c" "$bell/main/utilities/aes.c" \
    "$bell/external/civetweb/civetweb.c" \
    "$bell/external/nanopb/pb_common.c" "$bell/external/nanopb/pb_decode.c" \
    "$bell/external/nanopb/pb_encode.c" "$gen"/protobuf/*.pb.c \
    $(ls "$bell"/external/tremor/*.c | grep -v ivorbisfile_example)
rm -f "$out/libbell.a" && llvm-ar rcs "$out/libbell.a" "$out"/bell/*.o "$out"/bell-c/*.o
echo "deps: $out/libmbedtls.a $out/libbell.a"
