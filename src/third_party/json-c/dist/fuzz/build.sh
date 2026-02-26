#!/bin/bash -eu
# Copyright 2018 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

# This should be run from the top of the json-c source tree.

mkdir build
cd build
cmake -DBUILD_SHARED_LIBS=OFF ..
make -j$(nproc)

LIB=$(pwd)/libjson-c.a
cd ..

# These seem to be set externally, but let's assign defaults to
# make it possible to at least partially test this standalone.
: ${SRC:=$(dirname "$0")}
: ${OUT:=$SRC/out}
: ${CXX:=gcc}
: ${CXXFLAGS:=}

[ -d "$OUT" ] || mkdir "$OUT"
cp $SRC/*.dict $OUT/.

# XXX this doesn't seem to make much sense, since $SRC is presumably
# the "fuzz" directory, which is _inside_ the json-c repo, rather than
# the other way around, but I'm just preserving existing behavior. -erh
INCS=$SRC/json-c
# Compat when testing standalone
[ -e "${INCS}" ] || ln -s .. "${INCS}"

set -x
set -v
for f in $SRC/*_fuzzer.cc; do
    fuzzer=$(basename "$f" _fuzzer.cc)
    $CXX $CXXFLAGS -std=c++11 -I$INCS \
         $SRC/${fuzzer}_fuzzer.cc -o $OUT/${fuzzer}_fuzzer \
         -lFuzzingEngine $LIB
done
