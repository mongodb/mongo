#!/bin/sh

# Where to find the sources (only used to copy zstd.h)
ZSTD_SRC_ROOT="../../lib"

# Temporary compiled binary
OUT_FILE="tempbin"

# Optional temporary compiled WebAssembly
OUT_WASM="temp.wasm"

# Source files to compile using Emscripten.
IN_FILES="zstd.c examples/roundtrip.c"

# Emscripten build using emcc.
emscripten_emcc_build() {
  # Compile the the same example as above
  CC_FLAGS="-Wall -Wextra -Wshadow -Werror -Os -g0 -flto"
  emcc $CC_FLAGS -s WASM=1 -I. -o $OUT_WASM $IN_FILES
  # Did compilation work?
  if [ $? -ne 0 ]; then
    echo "Compiling ${IN_FILES}: FAILED"
    exit 1
  fi
  echo "Compiling ${IN_FILES}: PASSED"
  rm -f $OUT_WASM
}

# Emscripten build using docker.
emscripten_docker_build() {
  docker container run --rm \
    --volume $PWD:/code \
    --workdir /code \
    emscripten/emsdk:latest \
    emcc $CC_FLAGS -s WASM=1 -I. -o $OUT_WASM $IN_FILES
  # Did compilation work?
  if [ $? -ne 0 ]; then
      echo "Compiling ${IN_FILES} (using docker): FAILED"
    exit 1
  fi
  echo "Compiling ${IN_FILES} (using docker): PASSED"
  rm -f $OUT_WASM
}

# Try Emscripten build using emcc or docker.
try_emscripten_build() {
  which emcc > /dev/null
  if [ $? -eq 0 ]; then
    emscripten_emcc_build
    return $?
  fi

  which docker > /dev/null
  if [ $? -eq 0 ]; then
    emscripten_docker_build
    return $?
  fi

  echo "(Skipping Emscripten test)"
}

# Amalgamate the sources
./create_single_file_library.sh
# Did combining work?
if [ $? -ne 0 ]; then
  echo "Single file library creation script: FAILED"
  exit 1
fi
echo "Single file library creation script: PASSED"

# Copy the header to here (for the tests)
cp "$ZSTD_SRC_ROOT/zstd.h" zstd.h

# Compile the generated output
cc -Wall -Wextra -Werror -Wshadow -pthread -I. -Os -g0 -o $OUT_FILE zstd.c examples/roundtrip.c
# Did compilation work?
if [ $? -ne 0 ]; then
  echo "Compiling roundtrip.c: FAILED"
  exit 1
fi
echo "Compiling roundtrip.c: PASSED"

# Run then delete the compiled output
./$OUT_FILE
retVal=$?
rm -f $OUT_FILE
# Did the test work?
if [ $retVal -ne 0 ]; then
  echo "Running roundtrip.c: FAILED"
  exit 1
fi
echo "Running roundtrip.c: PASSED"

# Try Emscripten build if emcc or docker command is available.
try_emscripten_build

exit 0
