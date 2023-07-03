#!/bin/sh

set -e

println "+ good path"
zstdgrep "1234" file file.zst
println "+ bad path"
zstdgrep "1234" bad.zst
