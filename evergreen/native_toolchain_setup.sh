#!/usr/bin/env bash
# Installs the native (system) C++ compilers used by the weekly native-toolchain
set -o errexit
set -o verbose

export DEBIAN_FRONTEND=noninteractive

sudo apt-get update

# gcc-14 / g++-14 ship in the Ubuntu 24.04 archive.
sudo apt-get install -y gcc-14 g++-14

# clang-19 + lld-19: prefer the distro archive, fall back to apt.llvm.org if the
# image's repositories don't carry version 19.
if ! sudo apt-get install -y clang-19 lld-19; then
    echo "clang-19/lld-19 not available in the distro archive; installing via apt.llvm.org"
    wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    chmod +x /tmp/llvm.sh
    sudo /tmp/llvm.sh 19
    sudo apt-get install -y lld-19
fi

# gcc's -fuse-ld=lld resolves the linker by the name "ld.lld", but the package
# installs it as "ld.lld-19". Provide the unversioned name so the gcc build can
# find it. (clang locates its sibling ld.lld on its own.)
if [[ ! -e /usr/bin/ld.lld ]]; then
    lld_path="$(command -v ld.lld-19 || true)"
    if [[ -z "$lld_path" ]]; then
        echo "Could not find ld.lld-19 on PATH after installing lld-19"
        exit 1
    fi
    sudo ln -sf "$lld_path" /usr/bin/ld.lld
fi

# Sanity check that everything the build needs is present.
gcc-14 --version | head -1
g++-14 --version | head -1
clang-19 --version | head -1
clang++-19 --version | head -1
ld.lld --version | head -1
