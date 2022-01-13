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

flags="-DCMAKE_TOOLCHAIN_FILE=$curdir/cmake/toolchains/gcc.cmake -DCMAKE_C_FLAGS=\"-Werror -Wall -Wextra -Waddress -Waggregate-return -Wbad-function-cast -Wcast-align -Wdeclaration-after-statement -Wformat-security -Wformat-nonliteral -Wformat=2 -Wmissing-declarations -Wmissing-field-initializers -Wmissing-prototypes -Wnested-externs -Wno-unused-parameter -Wpointer-arith -Wredundant-decls -Wshadow -Wundef -Wunused -Wwrite-strings -O -fno-strict-aliasing -Wuninitialized\"
-DCMAKE_TOOLCHAIN_FILE=$curdir/cmake/toolchains/clang.cmake -DCMAKE_C_FLAGS=\"-Wall -Werror -Qunused-arguments -Wno-self-assign -Wno-parentheses-equality -Wno-array-bounds\""

options="-DHAVE_DIAGNOSTIC=1
-DENABLE_SHARED=0 -DENABLE_STATIC=1
-DENABLE_STATIC=0 -DENABLE_PYTHON=1
-DENABLE_SNAPPY=1 -DENABLE_ZLIB=1 -DENABLE_LZ4=1
-DHAVE_BUILTIN_EXTENSION_LZ4=1 -DHAVE_BUILTIN_EXTENSION_SNAPPY=1 -DHAVE_BUILTIN_EXTENSION_ZLIB=1
-DHAVE_DIAGNOSTIC=1 -DENABLE_PYTHON=1
-DENABLE_STRICT=1 -DENABLE_STATIC=1 -DENABLE_SHARED=0 -DWITH_PIC=1"

saved_IFS=$IFS
cr_IFS="
"

# This function may alter the current directory on failure
BuildTest() {
        echo "Building: $1, $2"
        rm -rf ./build || return 1
        mkdir build || return 1
        cd ./build
        eval $CMAKE_BIN "$1" "$2" \
                 -DCMAKE_INSTALL_PREFIX="$insdir" -G $GENERATOR ../. || return 1
        eval $GENERATOR_CMD $PARALLEL || return 1
        if [ "$GENERATOR" == "Unix\ Makefiles" ]; then
            $GENERATOR_CMD -C examples/c  VERBOSE=1 > /dev/null || return 1
        else
            $GENERATOR_CMD examples/c/all > /dev/null || return 1
        fi
        eval $GENERATOR_CMD install || return 1
        (echo $2 | grep "ENABLE_SHARED=0") && wt_build="--static" || wt_build=""
        cflags=`pkg-config wiredtiger $wt_build --cflags --libs`
        [ "$1"  == *"clang.cmake"* ] && compiler="clang" || compiler="cc"
        echo $compiler -o ./smoke ../examples/c/ex_smoke.c $cflags
        $compiler -o ./smoke ../examples/c/ex_smoke.c  $cflags|| return 1
        LD_LIBRARY_PATH="$insdir/lib:$insdir/lib64" ./smoke || return 1
        return 0
}

ecode=0
insdir=`pwd`/installed
export PKG_CONFIG_PATH="$insdir/lib/pkgconfig:$insdir/lib64/pkgconfig"
IFS="$cr_IFS"
for flag in $flags ; do
        for option in $options ; do
               cd "$curdir"
               IFS="$saved_IFS"
               if ! BuildTest "$flag" "$option" "$@"; then
                       ecode=1
                       echo "*** ERROR: $flag, $option"
               fi
               IFS="$cr_IFS"
       done
done
IFS=$saved_IFS
exit $ecode
