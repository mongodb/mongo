#!/bin/bash

# Note that the -x will be temporarily cancelled and reinstated below, so if
# you want to eliminate this, you'll need to eliminate it there too.
set -x
set -e

DIR="$(dirname $0)"
ABSDIR="$(cd $DIR; pwd)"
SOURCE="$(cd $DIR/../../../..; pwd)"

function usage() {
  echo "Usage: $0 [--dep] <variant>"
}

clean=1
platform=""
# 3 hours. OS X doesn't support the "sleep 3h" syntax.
TIMEOUT=10800
while [ $# -gt 1 ]; do
    case "$1" in
        --dep)
            shift
            clean=""
            ;;
        --platform)
            shift
            platform="$1"
            shift
            ;;
        --timeout)
            shift
            TIMEOUT="$1"
            shift
            ;;
        *)
            echo "Invalid arguments" >&2
            usage
            exit 1
            ;;
    esac
done

VARIANT=$1

# 'generational' is being retired in favor of 'compacting', but we need to
# decouple the landings.
if [[ "$VARIANT" = "generational" ]]; then
    VARIANT=compacting
fi

if [ ! -f "$ABSDIR/variants/$VARIANT" ]; then
    echo "Could not find variant '$VARIANT'"
    usage
    exit 1
fi

(cd "$SOURCE/js/src"; autoconf-2.13 || autoconf2.13 || autoconf213)

TRY_OVERRIDE=$SOURCE/js/src/config.try
if [ -r $TRY_OVERRIDE ]; then
  CONFIGURE_ARGS="$(cat "$TRY_OVERRIDE")"
else
  CONFIGURE_ARGS="$(cat "$ABSDIR/variants/$VARIANT")"
fi

OBJDIR="${OBJDIR:-$SOURCE/obj-spider}"

if [ -n "$clean" ]; then
  [ -d "$OBJDIR" ] && rm -rf "$OBJDIR"
  mkdir "$OBJDIR"
else
  [ -d "$OBJDIR" ] || mkdir "$OBJDIR"
fi
cd "$OBJDIR"

echo "OBJDIR is $OBJDIR"

USE_64BIT=false

if [[ "$OSTYPE" == darwin* ]]; then
  USE_64BIT=true
  if [ "$VARIANT" = "arm-sim-osx" ]; then
    USE_64BIT=false
  fi
  source "$ABSDIR/macbuildenv.sh"
elif [ "$OSTYPE" = "linux-gnu" ]; then
  if [ -n "$AUTOMATION" ]; then
      GCCDIR="${GCCDIR:-/tools/gcc-4.7.2-0moz1}"
      CONFIGURE_ARGS="$CONFIGURE_ARGS --with-ccache"
  fi
  UNAME_M=$(uname -m)
  MAKEFLAGS=-j4
  if [ "$VARIANT" = "arm-sim" ]; then
    USE_64BIT=false
  elif [ "$VARIANT" = "arm64-sim" ]; then
    USE_64BIT=true
  else
    case "$platform" in
    linux64)
      USE_64BIT=true
      ;;
    linux64-debug)
      USE_64BIT=true
      ;;
    linux)
      USE_64BIT=false
      ;;
    linux-debug)
      USE_64BIT=false
      ;;
    *)
      if [ "$UNAME_M" = "x86_64" ]; then
        USE_64BIT=true
      fi
      ;;
    esac
  fi

  if [ "$UNAME_M" != "arm" ] && [ -n "$AUTOMATION" ]; then
    export CC=$GCCDIR/bin/gcc
    export CXX=$GCCDIR/bin/g++
    if $USE_64BIT; then
      export LD_LIBRARY_PATH=$GCCDIR/lib64
    else
      export LD_LIBRARY_PATH=$GCCDIR/lib
    fi
  fi
elif [ "$OSTYPE" = "msys" ]; then
  case "$platform" in
  win64*)
    USE_64BIT=true
    ;;
  *)
    USE_64BIT=false
    ;;
  esac
  MAKE=${MAKE:-mozmake}
  source "$ABSDIR/winbuildenv.sh"
fi

MAKE=${MAKE:-make}

if $USE_64BIT; then
  NSPR64="--enable-64bit"
  if [ "$OSTYPE" = "msys" ]; then
    CONFIGURE_ARGS="$CONFIGURE_ARGS --target=x86_64-pc-mingw32 --host=x86_64-pc-mingw32"
  fi
else
  NSPR64=""
  if [ "$OSTYPE" == darwin* ]; then
    export CC="${CC:-/usr/bin/clang} -arch i386"
    export CXX="${CXX:-/usr/bin/clang++} -arch i386"
  elif [ "$OSTYPE" != "msys" ]; then
    export CC="${CC:-/usr/bin/gcc} -m32"
    export CXX="${CXX:-/usr/bin/g++} -m32"
    export AR=ar
  fi
  if [ "$OSTYPE" = "linux-gnu" ]; then
    if [ "$UNAME_M" != "arm" ] && [ -n "$AUTOMATION" ]; then
      CONFIGURE_ARGS="$CONFIGURE_ARGS --target=i686-pc-linux --host=i686-pc-linux"
    fi
  fi
fi

$SOURCE/js/src/configure $CONFIGURE_ARGS --enable-nspr-build --prefix=$OBJDIR/dist || exit 2
$MAKE -s -w -j4 || exit 2
cp -p $SOURCE/build/unix/run-mozilla.sh $OBJDIR/dist/bin

COMMAND_PREFIX=''

# On Linux, disable ASLR to make shell builds a bit more reproducible.
if type setarch >/dev/null 2>&1; then
    COMMAND_PREFIX="setarch $(uname -m) -R "
fi

RUN_JSTESTS=true
RUN_JITTEST=true
RUN_JSAPITESTS=true

PARENT=$$

# Spawn off a child process, detached from any of our fds, that will kill us after a timeout.
# To report the timeout, catch the signal in the parent before exiting.
sh -c "sleep $TIMEOUT; kill -INT $PARENT" <&- >&- 2>&- &
KILLER=$!
disown %1
set +x
trap "echo 'TEST-UNEXPECTED-FAIL | autospider.sh $TIMEOUT timeout | ignore later failures' >&2; exit 1" INT
set -x

# If we do *not* hit that timeout, kill off the spawned process on a regular exit.
trap "kill $KILLER" EXIT

if [[ "$VARIANT" = "rootanalysis" ]]; then
    export JS_GC_ZEAL=7
    export JSTESTS_EXTRA_ARGS=--jitflags=debug
elif [[ "$VARIANT" = "compacting" ]]; then
    export JS_GC_ZEAL=14

    # Ignore timeouts from tests that are known to take too long with this zeal mode.
    # Run jittests with reduced jitflags option (3 configurations).
    # Run jstests with default jitflags option (1 configuration).
    export JITTEST_EXTRA_ARGS="--jitflags=debug --ignore-timeouts=$ABSDIR/cgc-jittest-timeouts.txt"
    export JSTESTS_EXTRA_ARGS="--exclude-file=$ABSDIR/cgc-jstests-slow.txt"

    case "$platform" in
    win*)
        RUN_JSTESTS=false
    esac
elif [[ "$VARIANT" = "warnaserr" ||
        "$VARIANT" = "warnaserrdebug" ||
        "$VARIANT" = "plain" ]]; then
    export JSTESTS_EXTRA_ARGS=--jitflags=all
elif [[ "$VARIANT" = "arm-sim" ||
        "$VARIANT" = "arm-sim-osx" ||
        "$VARIANT" = "plaindebug" ]]; then
    export JSTESTS_EXTRA_ARGS=--jitflags=debug
elif [[ "$VARIANT" = arm64* ]]; then
    # The ARM64 JIT is not yet fully functional, and asm.js does not work.
    # Just run "make check" and jsapi-tests.
    RUN_JITTEST=false
    RUN_JSTESTS=false
fi

$COMMAND_PREFIX $MAKE check || exit 1

RESULT=0

if $RUN_JITTEST; then
    $COMMAND_PREFIX $MAKE check-jit-test || RESULT=$?
fi
if $RUN_JSAPITESTS; then
    $COMMAND_PREFIX $OBJDIR/dist/bin/jsapi-tests || RESULT=$?
fi
if $RUN_JSTESTS; then
    $COMMAND_PREFIX $MAKE check-jstests || RESULT=$?
fi

exit $RESULT
