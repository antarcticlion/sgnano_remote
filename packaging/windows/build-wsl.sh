#!/usr/bin/env bash
# WSL から Windows x64 の sgnano_remote と NSIS インストーラを組む（2026-11-15）。
# 必要なもの:
#   sudo apt install g++-mingw-w64-x86-64 nsis
#   Qt for Windows MinGW（既定 ~/Qt/6.2.4/mingw_64）
#   Linux 側の Qt 6 のホストツール（moc など。QT_HOST_PATH で指定）
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QT_DIR=${QT_DIR:-"$HOME/Qt/6.2.4/mingw_64"}
QT_HOST_PATH=${QT_HOST_PATH:-/usr/lib/x86_64-linux-gnu/cmake}
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-windows"}
STAGE_DIR="$BUILD_DIR/stage"
VERSION=${VERSION:-0.1.0}
# Qt の Windows DLL は GCC 11.2 でビルドされている。アプリ側は CMake で
# libstdc++/libgcc を静的に結ぶので、WSL の MinGW（GCC 10 でも可）で組める。
# MINGW_BIN を指定すると、その bin を先に使う。
if [ -n "${MINGW_BIN:-}" ]; then
    export PATH="$MINGW_BIN:$PATH"
fi

command -v x86_64-w64-mingw32-g++ >/dev/null || {
    echo "x86_64-w64-mingw32-g++ がありません（sudo apt install g++-mingw-w64-x86-64、または MINGW_BIN を指定）" >&2
    exit 1
}
if ! command -v makensis >/dev/null; then
    if [ -x "$HOME/.local/nsis/usr/bin/makensis" ]; then
        export PATH="$HOME/.local/nsis/usr/bin:$PATH"
        export NSISDIR="$HOME/.local/nsis/usr/share/nsis"
    else
        echo "makensis がありません（sudo apt install nsis）" >&2
        exit 1
    fi
fi
if [ -n "${NSISDIR:-}" ]; then
    export NSISDIR
fi
[ -f "$QT_DIR/lib/cmake/Qt6/Qt6Config.cmake" ] || {
    echo "Qt for Windows が見つかりません: $QT_DIR" >&2
    exit 1
}

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/packaging/windows/mingw-w64.cmake" \
    -DCMAKE_PREFIX_PATH="$QT_DIR" \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST_PATH"
cmake --build "$BUILD_DIR"

rm -rf "$STAGE_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR"

EXE="$STAGE_DIR/sgnano_remote.exe"
[ -f "$EXE" ] || { echo "stage に exe がありません: $EXE" >&2; exit 1; }

DEPLOY="$QT_DIR/bin/windeployqt.exe"
if [ -f "$DEPLOY" ]; then
    chmod +x "$DEPLOY" 2>/dev/null || true
    if ! "$DEPLOY" --release --compiler-runtime --no-translations --dir "$STAGE_DIR" "$EXE"; then
        echo "windeployqt を直接実行できません。wine がある場合は wine で試します" >&2
        command -v wine >/dev/null || { echo "wine もありません" >&2; exit 1; }
        wine "$DEPLOY" --release --compiler-runtime --no-translations --dir "$STAGE_DIR" "$EXE"
    fi
else
    echo "windeployqt.exe がありません: $DEPLOY" >&2
    exit 1
fi

# windeployqt が別の MinGW の実行時 DLL を拾うことがあるので、Qt と同じ GCC 11.2 のものにそろえる
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    if [ -f "$QT_DIR/bin/$dll" ]; then
        cp -f "$QT_DIR/bin/$dll" "$STAGE_DIR/"
    fi
done

pushd "$ROOT/packaging/windows" >/dev/null
makensis "-DSTAGE=$STAGE_DIR" "-DVERSION=$VERSION" installer.nsi
popd >/dev/null
mv "$ROOT/packaging/windows/sgnano_remote_setup_$VERSION.exe" "$BUILD_DIR/"

echo "完成: $BUILD_DIR/sgnano_remote_setup_$VERSION.exe"
