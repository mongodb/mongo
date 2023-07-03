#!/bin/sh

# Where to find the sources
ZSTD_SRC_ROOT="../../lib"

# Amalgamate the sources
echo "Amalgamating files..."
# Using the faster Python script if we have 3.8 or higher
if python3 -c 'import sys; assert sys.version_info >= (3,8)' 2>/dev/null; then
  ./combine.py -r "$ZSTD_SRC_ROOT" -x legacy/zstd_legacy.h -o zstd.c zstd-in.c
else
  ./combine.sh -r "$ZSTD_SRC_ROOT" -x legacy/zstd_legacy.h -o zstd.c zstd-in.c
fi
# Did combining work?
if [ $? -ne 0 ]; then
  echo "Combine script: FAILED"
  exit 1
fi
echo "Combine script: PASSED"
