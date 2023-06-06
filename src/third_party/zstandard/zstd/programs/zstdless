#!/bin/sh

zstd=${ZSTD:-zstd}

# TODO: Address quirks and bugs tied to old versions of less, provide a mechanism to pass flags directly to zstd

export LESSOPEN="|-${zstd} -cdfq %s"
exec less "$@"
