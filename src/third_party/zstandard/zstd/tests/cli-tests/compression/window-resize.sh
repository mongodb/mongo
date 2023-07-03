#!/bin/sh
datagen -g1G > file
zstd --long=30 -1 --single-thread --no-content-size -f file
zstd -l -v file.zst

# We want to ignore stderr (its outputting "*** zstd command line interface
# 64-bits v1.5.3, by Yann Collet ***")

rm file file.zst
