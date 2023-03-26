#!/bin/sh

export LD_LIBRARY_PATH=/opt/bin:/opt/tools/voidstar/lib:$LD_LIBRARY_PATH
cd ./bin/test/format
./t "$@" 2>&1