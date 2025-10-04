#!/bin/sh

echo "some data" > file

println "+ zstd --memory=32LB file"
zstd --memory=32LB file && die "Should not allow bogus suffix"
println "+ zstd --memory=32LiB file"
zstd --memory=32LiB file && die "Should not allow bogus suffix"
println "+ zstd --memory=32A file"
zstd --memory=32A file && die "Should not allow bogus suffix"
println "+ zstd --memory=32r82347dn83 file"
zstd --memory=32r82347dn83 file && die "Should not allow bogus suffix"
println "+ zstd --memory=32asbdf file"
zstd --memory=32asbdf file && die "Should not allow bogus suffix"
println "+ zstd --memory=hello file"
zstd --memory=hello file && die "Should not allow non-numeric parameter"
println "+ zstd --memory=1 file"
zstd -q --memory=1 file && die "Should allow numeric parameter without suffix"
rm file.zst
println "+ zstd --memory=1K file"
zstd -q --memory=1K file && die "Should allow numeric parameter with expected suffix"
rm file.zst
println "+ zstd --memory=1KB file"
zstd -q --memory=1KB file && die "Should allow numeric parameter with expected suffix"
rm file.zst
println "+ zstd --memory=1KiB file"
zstd -q --memory=1KiB file && die "Should allow numeric parameter with expected suffix"
rm file.zst
println "+ zstd --memory=1M file"
zstd -q --memory=1M file && die "Should allow numeric parameter with expected suffix"
rm file.zst
println "+ zstd --memory=1MB file"
zstd -q --memory=1MB file && die "Should allow numeric parameter with expected suffix"
rm file.zst
println "+ zstd --memory=1MiB file"
zstd -q --memory=1MiB file && die "Should allow numeric parameter with expected suffix"
rm file.zst

rm file
exit 0
