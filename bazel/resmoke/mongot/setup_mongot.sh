#!/usr/bin/env bash
# Wrapper for db-contrib-tool setup-mongot-repro-env invoked from a Bazel action.

set -euo pipefail

root=$PWD
tool=$1
version=$2
url=$3
output_dir=$4

log=$(mktemp)
tmp=$(mktemp -d)
trap 'rm -rf "$log" "$tmp"' EXIT

if ! (
    cd "$tmp"
    if [[ -n "$url" ]]; then
        # Downstream 10gen/mongot patches supply a prebuilt tarball URL.
        curl -fsSL --retry 3 "$url" | tar xz
    else
        case "$OSTYPE" in
        linux-gnu*) platform="linux" ;;
        darwin*) platform="macos" ;;
        *)
            echo "mongot is only supported on linux and mac, not ${OSTYPE}"
            exit 1
            ;;
        esac
        # macos arm64 is not supported by mongot, but the macos x86_64 binary
        # runs on it successfully via Rosetta.
        arch="x86_64"
        if [[ "$(uname -m)" == aarch64* && "$platform" == "linux" ]]; then
            arch="aarch64"
        fi
        "$root/$tool" setup-mongot-repro-env "$version" \
            --platform="$platform" \
            --architecture="$arch" \
            --installDir=.
    fi
    # Hack to remove a BUILD.bazel file that can be lying around in mongot.
    rm -f mongot-localdev/bin/jdk/BUILD.bazel
) >"$log" 2>&1; then
    cat "$log" >&2
    exit 1
fi

# db-contrib-tool creates <installDir>/mongot-localdev; the output directory
# itself is named mongot-localdev, so move the contents up one level.
cp -a "$tmp/mongot-localdev/." "$root/$output_dir/"
