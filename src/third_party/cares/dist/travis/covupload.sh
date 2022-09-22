#!/bin/sh
set -e
if [ "$BUILD_TYPE" = "coverage" ]; then
    coveralls --gcov-options '\-lp'
fi
