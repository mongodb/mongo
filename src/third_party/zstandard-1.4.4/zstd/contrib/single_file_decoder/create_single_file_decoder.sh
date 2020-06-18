#!/bin/sh

# Where to find the sources
ZSTD_SRC_ROOT="../../lib"


# Amalgamate the sources
echo "Amalgamating files... this can take a while"
./combine.sh -r "$ZSTD_SRC_ROOT" -r "$ZSTD_SRC_ROOT/common" -r "$ZSTD_SRC_ROOT/decompress" -o zstddeclib.c zstddeclib-in.c
# Did combining work?
if [ $? -ne 0 ]; then
  echo "Combine script: FAILED"
  exit 1
fi
echo "Combine script: PASSED"
