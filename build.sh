#!/usr/bin/env bash
# Build the 32-bit sampcmd.exe with the MinGW-w64 cross toolchain.
#
#   ./build.sh                 configure + build into ./build
#   ./build.sh --deploy DIR    also copy sampcmd.exe into game directory DIR
#   ./build.sh --clean         remove the build directory first
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$here/build"
triple="${MINGW_TRIPLE:-i686-w64-mingw32}"
deploy_dir=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)  rm -rf "$build_dir"; shift ;;
    --deploy) deploy_dir="${2:?--deploy needs a directory}"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if ! command -v "${triple}-g++" >/dev/null 2>&1; then
  cat >&2 <<EOF
error: MinGW-w64 32-bit toolchain not found (${triple}-g++).

Install it, then re-run this script:

  Arch / CachyOS : sudo pacman -S mingw-w64-gcc
  Debian/Ubuntu  : sudo apt install g++-mingw-w64-i686 cmake
  Fedora         : sudo dnf install mingw32-gcc-c++ cmake

(If your g++ is suffixed, e.g. ${triple}-g++-posix, set MINGW_TRIPLE.)
EOF
  exit 1
fi

echo ">> using toolchain: ${triple}-g++ ($(${triple}-g++ -dumpversion))"

cmake -B "$build_dir" -S "$here" \
  -DCMAKE_TOOLCHAIN_FILE="$here/cmake/toolchain-mingw-i686.cmake" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$build_dir" --parallel

exe="$build_dir/sampcmd.exe"
echo ">> built: $exe"
file "$exe" || true

if [[ -n "$deploy_dir" ]]; then
  if [[ ! -d "$deploy_dir" ]]; then
    echo "error: deploy dir does not exist: $deploy_dir" >&2; exit 1
  fi
  # Never clobber the user's original without a backup.
  if [[ -f "$deploy_dir/sampcmd.exe" && ! -f "$deploy_dir/sampcmd.exe.orig" ]]; then
    cp -v "$deploy_dir/sampcmd.exe" "$deploy_dir/sampcmd.exe.orig"
    echo ">> backed up original to sampcmd.exe.orig"
  fi
  cp -v "$exe" "$deploy_dir/sampcmd.exe"
  echo ">> deployed to $deploy_dir/sampcmd.exe"
fi
