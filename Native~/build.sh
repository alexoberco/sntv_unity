#!/bin/sh
# Builds the Android receiver (sntv_android.c) with the NDK that ships with the Unity Editor.
#   Native~/build.sh        (from anywhere)      UNITY=<editor dir> to pick another version
# Output: Runtime/Plugins/Android/arm64-v8a/libsntv.so, which Unity packs into the APK.
set -e
cd "$(dirname "$0")/.."
UNITY=${UNITY:-$(ls -d "$HOME"/Unity/Hub/Editor/6000.* 2>/dev/null | sort -V | tail -1)}
NDK=${NDK:-$UNITY/Editor/Data/PlaybackEngines/AndroidPlayer/NDK}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang
[ -x "$CC" ] || { echo "No NDK clang at $CC (set UNITY= or NDK=)" >&2; exit 1; }
OUT=Runtime/Plugins/Android/arm64-v8a
mkdir -p $OUT
$CC -O2 -fPIC -shared -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -INative~ \
    -o $OUT/libsntv.so Native~/sntv_android.c \
    -lmediandk -landroid -lEGL -lGLESv3 -llog -Wl,--no-undefined -Wl,-z,max-page-size=16384
echo "OK: $OUT/libsntv.so"
