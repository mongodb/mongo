#!/bin/sh

println "+ zstd -r * --output-dir-mirror=\"\""
zstd -r * --output-dir-mirror="" && die "Should not allow empty output dir!"
println "+ zstd -r * --output-dir-flat=\"\""
zstd -r * --output-dir-flat="" && die "Should not allow empty output dir!"
exit 0
