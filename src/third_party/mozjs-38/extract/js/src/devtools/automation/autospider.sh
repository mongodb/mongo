#!/bin/bash
set -e
set -x

DIR="$(dirname $0)"
ABSDIR="$(cd $DIR; pwd)"
SOURCE="$(cd $DIR/../../../..; pwd)"

function usage() {
  echo "Usage: $0 [--dep] <variant>"
}

clean=1
platform=""
TIMEOUT=3h
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
elif [ "$OSTYPE" = "linux-gnu" ]; then
  if [ -n "$AUTOMATION" ]; then
      GCCDIR="${GCCDIR:-/tools/gcc-4.7.2-0moz1}"
      CONFIGURE_ARGS="$CONFIGURE_ARGS --with-ccache"
  fi
  UNAME_M=$(uname -m)
  MAKEFLAGS=-j4
  if [ "$VARIANT" = "arm-sim" ]; then
    USE_64BIT=false
  elif [ "$UNAME_M" = "x86_64" ]; then
    USE_64BIT=true
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
  USE_64BIT=false
  if [ "$platform" = "win64" ]; then
      USE_64BIT=true
  fi
  MAKE=${MAKE:-mozmake}
  source "$ABSDIR/winbuildenv.sh"
fi

MAKE=${MAKE:-make}

if $USE_64BIT; then
  NSPR64="--enable-64bit"
else
  NSPR64=""
  if [ "$OSTYPE" != "msys" ]; then
    export CC="${CC:-/usr/bin/gcc} -m32"
    export CXX="${CXX:-/usr/bin/g++} -m32"
    export AR=ar
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

PARENT=$$
sh -c "sleep $TIMEOUT; kill $PARENT" <&- >&- 2>&- &
KILLER=$!
disown %1
trap "kill $KILLER" EXIT

if [[ "$VARIANT" = "rootanalysis" ]]; then
    export JS_GC_ZEAL=7

elif [[ "$VARIANT" = "compacting" ]]; then
    export JS_GC_ZEAL=14

    # Ignore timeouts from tests that are known to take too long with this zeal mode
    export JITTEST_EXTRA_ARGS=--ignore-timeouts=$ABSDIR/cgc-jittest-timeouts.txt
    export JSTESTS_EXTRA_ARGS=--exclude-file=$ABSDIR/cgc-jstests-slow.txt

    case "$platform" in
    win*)
        RUN_JSTESTS=false
    esac
fi

if [[ "$VARIANT" = "warnaserr" ]]; then
    export JSTESTS_EXTRA_ARGS=--tbpl
fi

$COMMAND_PREFIX $MAKE check || exit 1
$COMMAND_PREFIX $MAKE check-jit-test || exit 1
$COMMAND_PREFIX $OBJDIR/dist/bin/jsapi-tests || exit 1
if $RUN_JSTESTS; then
    $COMMAND_PREFIX $MAKE check-jstests || exit 1
fi
