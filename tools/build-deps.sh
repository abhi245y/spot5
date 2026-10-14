#!/usr/bin/env bash
# spot5 - Builds the static libraries the app links: mbedTLS and bell's
# network layer, with the app's PS5 compiler wrapper.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Output: build/deps/libmbedtls.a, build/deps/libwebpdec.a, build/deps/libharfbuzz.a,
# build/deps/libbell.a
# ponytail: rebuilds everything each run (~1 min); move into the ninja graph if
# that starts to hurt.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_root="$root/.deps/native/ps5-payload-sdk"
bell="$root/third_party/cspot/cspot/bell"
out="$root/build/deps"

mbedtls_version=3.6.4
mbedtls_sha256=ec35b18a6c593cf98c3e30db8b98ff93e8940a8c4e690e66b41dfc011d678110
mbedtls_tar="$root/.deps/src/mbedtls-$mbedtls_version.tar.bz2"
mbedtls="$root/.deps/src/mbedtls-$mbedtls_version"

# libwebp, decoder only: some Spotify covers (user playlist images) are WebP.
# Release tarball signed by the WebP release key (6B0E6B70...B8232B5D),
# checked once by hand; the hash pins that verified file.
webp_version=1.4.0
webp_sha256=61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5
webp_tar="$root/.deps/src/libwebp-$webp_version.tar.gz"
webp="$root/.deps/src/libwebp-$webp_version"

# HarfBuzz: text shaping for scripts whose letters join and reorder (Malayalam,
# Devanagari, ...). One-file build (src/harfbuzz.cc), single-threaded: only the
# draw thread shapes text.
hb_version=11.2.1
hb_sha256=093714c8548a285094685f0bdc999e202d666b59eeb3df2ff921ab68b8336a49
hb_tar="$root/.deps/src/harfbuzz-$hb_version.tar.xz"
hb="$root/.deps/src/harfbuzz-$hb_version"

mkdir -p "$root/.deps/src" "$out"

# cspot and bell are vendored in third_party/cspot (VERSION says which commits).
echo "cspot/bell: $(grep -E '^(cspot|bell)' "$root/third_party/cspot/VERSION" | awk '{print $1"@"$2}' | tr '\n' ' ')"
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
cspot="$root/third_party/cspot/cspot"
# nanopb sources for cspot's .proto files, generated once and kept with the vendored
# cspot (nanopb 0.4.6's generator needs protobuf 4 on Python <= 3.12).
gen="$root/third_party/cspot/generated"
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

if [[ ! -f $webp_tar ]]; then
    curl -sSL -o "$webp_tar" \
        "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$webp_version.tar.gz"
fi
echo "$webp_sha256  $webp_tar" | sha256sum --check --quiet
[[ -d $webp ]] || tar -xzf "$webp_tar" -C "$root/.deps/src"
if [[ ! -f $hb_tar ]]; then
    curl -sSL -o "$hb_tar" \
        "https://github.com/harfbuzz/harfbuzz/releases/download/$hb_version/harfbuzz-$hb_version.tar.xz"
fi
echo "$hb_sha256  $hb_tar" | sha256sum --check --quiet
[[ -d $hb ]] || tar -xJf "$hb_tar" -C "$root/.deps/src"

compile "$out/mbedtls" mbedtls_flags "$mbedtls"/library/*.c
rm -f "$out/libmbedtls.a" && llvm-ar rcs "$out/libmbedtls.a" "$out"/mbedtls/*.o

# Everything under dec/dsp/utils compiles; the archive only lends the linker
# what WebPDecodeRGBA needs (encoder objects stay unused).
webp_flags=(-std=c11 -O2 -w -ffunction-sections -fdata-sections -I"$webp")
compile "$out/webp" webp_flags "$webp"/src/dec/*.c "$webp"/src/dsp/*.c "$webp"/src/utils/*.c
rm -f "$out/libwebpdec.a" && llvm-ar rcs "$out/libwebpdec.a" "$out"/webp/*.o

hb_flags=(-std=c++17 -O2 -w -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -DHB_NO_MT)
compile "$out/harfbuzz" hb_flags "$hb/src/harfbuzz.cc"
rm -f "$out/libharfbuzz.a" && llvm-ar rcs "$out/libharfbuzz.a" "$out"/harfbuzz/*.o

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
