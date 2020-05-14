#!/bin/sh

# Where to find the sources
ZSTD_SRC_ROOT="../../lib"

# Temporary compiled binary
OUT_FILE="tempbin"

# Optional temporary compiled WebAssembly
OUT_WASM="temp.wasm"

# Amalgamate the sources
./create_single_file_decoder.sh
# Did combining work?
if [ $? -ne 0 ]; then
  echo "Single file decoder creation script: FAILED"
  exit 1
fi
echo "Single file decoder creation script: PASSED"

# Compile the generated output
cc -Wall -Wextra -Werror -Os -g0 -o $OUT_FILE examples/simple.c
# Did compilation work?
if [ $? -ne 0 ]; then
  echo "Compiling simple.c: FAILED"
  exit 1
fi
echo "Compiling simple.c: PASSED"

# Run then delete the compiled output
./$OUT_FILE
retVal=$?
rm -f $OUT_FILE
# Did the test work?
if [ $retVal -ne 0 ]; then
  echo "Running simple.c: FAILED"
  exit 1
fi
echo "Running simple.c: PASSED"

# Is Emscripten available?
which emcc > /dev/null
if [ $? -ne 0 ]; then
  echo "(Skipping Emscripten test)"
else
  # Compile the Emscripten example
  CC_FLAGS="-Wall -Wextra -Werror -Os -g0 -flto --llvm-lto 3 -lGL -DNDEBUG=1"
  emcc $CC_FLAGS -s WASM=1 -o $OUT_WASM examples/emscripten.c
  # Did compilation work?
  if [ $? -ne 0 ]; then
    echo "Compiling emscripten.c: FAILED"
    exit 1
  fi
  echo "Compiling emscripten.c: PASSED"
  rm -f $OUT_WASM
fi

exit 0
