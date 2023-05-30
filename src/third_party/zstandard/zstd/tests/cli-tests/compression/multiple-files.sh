#!/bin/sh
set -e

# setup
echo "file1" > file1
echo "file2" > file2

echo "Test zstd ./file1 - file2"
rm -f ./file*.zst
echo "stdin" | zstd ./file1 - ./file2 | zstd -d
cat file1.zst | zstd -d
cat file2.zst | zstd -d

echo "Test zstd -d ./file1.zst - file2.zst"
rm ./file1 ./file2
echo "stdin" | zstd - | zstd -d ./file1.zst - file2.zst
cat file1
cat file2

echo "zstd -d ./file1.zst - file2.zst -c"
echo "stdin" | zstd | zstd -d ./file1.zst - file2.zst -c
