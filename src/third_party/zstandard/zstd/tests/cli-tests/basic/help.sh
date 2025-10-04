#!/bin/sh

set -e

println "+ zstd -h"
zstd -h
println "+ zstd -H"
zstd -H
println "+ zstd --help"
zstd --help
