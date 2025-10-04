#!/bin/sh

# Temporary compiled binary
OUT_FILE="tempbin"

# Optional temporary compiled WebAssembly
OUT_WASM="temp.wasm"

# Source files to compile using Emscripten.
IN_FILES="examples/emscripten.c"

# Emscripten build using emcc.
emscripten_emcc_build() {
  # Compile the same example as above
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
./create_single_file_decoder.sh
# Did combining work?
if [ $? -ne 0 ]; then
  echo "Single file decoder creation script: FAILED"
  exit 1
fi
echo "Single file decoder creation script: PASSED"

# Compile the generated output
cc -Wall -Wextra -Wshadow -Werror -Os -g0 -o $OUT_FILE examples/simple.c
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

# Try Emscripten build if emcc or docker command is available.
try_emscripten_build

exit 0
