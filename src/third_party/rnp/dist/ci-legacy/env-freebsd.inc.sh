#!/usr/bin/env sh

export PATH="/usr/local/bin:$PATH"
export MAKE=gmake
export SUDO=sudo

: "${CORES:=$(sysctl -n hw.ncpu)}"
export CORES
