#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
BUILD="$ROOT/build-android/arm64-v8a"
export PYTHONUNBUFFERED=1
export MSYS_NO_PATHCONV=1
export PYTHONPATH="C:/Users/taska/Desktop/新建文件夹/toolchain/meson-patched"
mapfile -t OPTS < "$ROOT/android/options.ini"
python -m mesonbuild.mesonmain setup "$BUILD" "$ROOT" \
    --cross-file "$BUILD/cross.ini" \
    --prefix /taisei \
    --buildtype release \
    --wrap-mode=default \
    "${OPTS[@]}" "$@"
