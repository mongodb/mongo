#!/usr/bin/env bash
# Repackages an official LLVM macOS release tarball into the stripped archive
# consumed by //bazel/toolchains/cc/mongo_apple:mongo_apple_toolchain.bzl.
#
# The upstream archives are ~1.5GB compressed / ~7.6GB extracted. The native
# macOS toolchain only needs the compiler, linker, binutils, the clang resource
# directory, and the libc++ headers, so everything else is dropped.
#
# Usage:
#   package_llvm.sh <LLVM-X.Y.Z-macOS-{ARM64,X64}.tar.xz> <output_dir>
#
# Upstream archives come from https://github.com/llvm/llvm-project/releases.
# Upload the resulting archive to s3://mdb-build-public/toolchains/
# and update the URL/sha256 in mongo_apple_toolchain.bzl.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <LLVM-X.Y.Z-macOS-ARCH.tar.xz> <output_dir>" >&2
    exit 1
fi

src_archive="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
out_dir="$(mkdir -p "$2" && cd "$2" && pwd -P)"

# LLVM-19.1.7-macOS-ARM64.tar.xz -> LLVM-19.1.7-macOS-ARM64 / 19.1.7 / arm64
name="$(basename "$src_archive" .tar.xz)"
version="$(echo "$name" | sed -E 's/^LLVM-([0-9.]+)-macOS-.*$/\1/')"
major="${version%%.*}"
case "$name" in
*-ARM64) arch="arm64" ;;
*-X64) arch="x86_64" ;;
*)
    echo "unrecognized archive name: $name" >&2
    exit 1
    ;;
esac

out_name="llvm-${version}-macos-${arch}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Extracting $src_archive"
# Extract and stage in separate directories: the upstream and output names only
# differ by case, which collides on case-insensitive filesystems.
mkdir -p "$work/src" "$work/dst"
tar -xJf "$src_archive" -C "$work/src"
src="$work/src/$name"
dst="$work/dst/$out_name"

# Symlinks (clang -> clang-19, llvm-strip -> llvm-objcopy, ...) are preserved.
tools=(
    "clang-${major}" clang clang++ clang-cpp
    lld ld.lld ld64.lld
    llvm-ar llvm-ranlib
    llvm-cov
    llvm-dwp
    llvm-nm
    llvm-objcopy llvm-strip
    llvm-objdump
    llvm-profdata
    llvm-symbolizer
)
mkdir -p "$dst/bin" "$dst/include" "$dst/lib"
for tool in "${tools[@]}"; do
    cp -a "$src/bin/$tool" "$dst/bin/"
done
cp -a "$src/include/c++" "$dst/include/"
cp -a "$src/lib/clang" "$dst/lib/"

echo "Creating $out_dir/$out_name.tar.xz"
tar -C "$work/dst" -cf - "$out_name" | xz -T0 -9 >"$out_dir/$out_name.tar.xz"
shasum -a 256 "$out_dir/$out_name.tar.xz"
