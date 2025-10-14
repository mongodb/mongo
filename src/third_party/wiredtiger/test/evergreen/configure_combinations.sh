#!/bin/bash

: ${CMAKE_BIN:=cmake}

for i in "$@"; do
  case $i in
    -g=*|--generator=*)
      GENERATOR="${i#*=}"
      shift # past argument=value
      ;;
    -j=*|--parallel=*)
      PARALLEL="-j ${i#*=}"
      shift # past argument=value
      ;;
    *)
      # unknown option
      ;;
  esac
done

if [ -z "${GENERATOR}" ]; then
    GENERATOR="Unix Makefiles"
fi
if [ "$GENERATOR" != "Ninja" ] && [ "$GENERATOR" != "Unix Makefiles" ]; then
    echo "Invalid build generator: $GENERATOR. Valid options 'Ninja', 'Unix Makefiles'"
fi

if [ "$GENERATOR" == "Unix Makefiles" ]; then
    GENERATOR=$(echo $GENERATOR | sed -e 's/ /\\ /')
    GENERATOR_CMD="make"
else
    GENERATOR_CMD="ninja"
fi

cd $(git rev-parse --show-toplevel)
echo `pwd`

curdir=`pwd`

compilers=(
    "-DCMAKE_TOOLCHAIN_FILE=$curdir/cmake/toolchains/mongodbtoolchain_stable_gcc.cmake"
    "-DCMAKE_TOOLCHAIN_FILE=$curdir/cmake/toolchains/mongodbtoolchain_stable_clang.cmake"
)

options=(
    "-DHAVE_DIAGNOSTIC=ON"
    "-DENABLE_SHARED=OFF -DENABLE_STATIC=ON"
    "-DENABLE_STATIC=OFF -DENABLE_PYTHON=ON"
    "-DENABLE_SNAPPY=ON -DENABLE_ZLIB=ON -DENABLE_LZ4=ON"
    "-DHAVE_BUILTIN_EXTENSION_LZ4=ON -DHAVE_BUILTIN_EXTENSION_SNAPPY=ON -DHAVE_BUILTIN_EXTENSION_ZLIB=ON"
    "-DHAVE_DIAGNOSTIC=ON -DENABLE_PYTHON=ON"
    "-DENABLE_STATIC=ON -DENABLE_SHARED=OFF -DWITH_PIC=ON"
)

always="-DENABLE_STRICT=ON -DENABLE_COLORIZE_OUTPUT=OFF"

saved_IFS=$IFS
cr_IFS="
"

# Function to discover compiler path using a temporary CMake project
discover_compiler() {
    local toolchain_file="$1"
    local temp_dir=$(mktemp -d)
    local cmake_file="$temp_dir/CMakeLists.txt"
    local original_dir=$(pwd)

    # Create temporary CMake project
    cat > "$cmake_file" << 'EOF'
cmake_minimum_required(VERSION 3.10)
project(CompilerPath)
# Write the compiler path to a file
file(WRITE ${CMAKE_BINARY_DIR}/compiler_path.txt "${CMAKE_C_COMPILER}")
EOF

    # Change to temp directory and run CMake
    cd "$temp_dir"
    if eval $CMAKE_BIN "$toolchain_file" -B . -S . > /dev/null 2>&1; then
        if [ -f "./compiler_path.txt" ]; then
            discovered_compiler=$(cat "./compiler_path.txt")
            # Clean up
            cd "$original_dir"
            rm -rf "$temp_dir"
            echo "$discovered_compiler"
            return 0
        fi
    fi

    # Clean up on failure
    cd "$original_dir"
    rm -rf "$temp_dir"
    return 1
}

# This function may alter the current directory on failure
BuildTest() {
        local toolchain="$1"
        local options="$2"
        local compiler_path="$3"
        echo "Building: $toolchain, $options"
        rm -rf ./build || return 1
        mkdir build || return 1
        cd ./build
        eval $CMAKE_BIN "$toolchain" "$options" \
                 -DCMAKE_INSTALL_PREFIX="$insdir" -G $GENERATOR ../. || return 1
        eval $GENERATOR_CMD $PARALLEL || return 1
        if [ "$GENERATOR" == "Unix\ Makefiles" ]; then
            $GENERATOR_CMD -C examples/c  VERBOSE=1 > /dev/null || return 1
        else
            $GENERATOR_CMD examples/c/all > /dev/null || return 1
        fi
        eval $GENERATOR_CMD install || return 1
        (echo $options | grep "ENABLE_SHARED=OFF") && wt_build="--static" || wt_build=""
        cflags=`pkg-config wiredtiger $wt_build --cflags --libs`

        echo $compiler_path -o ./smoke ../examples/c/ex_smoke.c $cflags
        $compiler_path -o ./smoke ../examples/c/ex_smoke.c  $cflags|| return 1
        LD_LIBRARY_PATH="$insdir/lib:$insdir/lib64" ./smoke || return 1
        return 0
}

ecode=0
insdir=`pwd`/installed
export PKG_CONFIG_PATH="$insdir/lib/pkgconfig:$insdir/lib64/pkgconfig"
IFS="$cr_IFS"
for compiler in "${compilers[@]}" ; do
        # Discover compiler path once per toolchain
        compiler_path=$(discover_compiler "$compiler")
        echo "Using compiler: $compiler_path for toolchain: $compiler"

        for option in "${options[@]}" ; do
               cd "$curdir"
               IFS="$saved_IFS"
               option="$option $always"
               if ! BuildTest "$compiler" "$option" "$compiler_path" "$@"; then
                       ecode=1
                       echo "*** ERROR: $compiler, $option"
               fi
               IFS="$cr_IFS"
       done
done
IFS=$saved_IFS
exit $ecode
