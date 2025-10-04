#!/bin/sh

set -e

println "+ good path"
zstdless file.zst
println "+ pass parameters"
zstdless -N file.zst # This parameter does not produce line #s when piped, but still serves to test that the flag went to less and not zstd
println "+ bad path"
zstdless bad.zst >&2
