#!/bin/sh

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 SPEC_DIRECTORY" >&2
  exit 1
fi
if ! [ -e "$1" ]; then
  echo "Directory $1 not found" >&2
  exit 1
fi
if ! [ -d "$1" ]; then
  echo "$1 not a directory" >&2
  exit 1
fi
if ! [ -f "tla2tools.jar" ]; then
  echo "No tla2tools.jar, run download-tlc.sh first"
  exit 1
fi

TLA_FILE="MC$1.tla"
if ! [ -f "$1/$TLA_FILE" ]; then
  echo "$1/$TLA_FILE does not exist" >&2
  exit 1
fi

# Count CPUs. Linux has cpuinfo, Mac has sysctl, otherwise use a default.
if [ -e "/proc/cpuinfo" ]; then
  WORKERS=$(grep -c processor /proc/cpuinfo)
elif ! WORKERS=$(sysctl -n hw.logicalcpu); then
  WORKERS=8 # default
fi

cd "$1"
# Defer liveness checks to the end with -lncheck, for speed.
java -XX:+UseParallelGC -cp ../tla2tools.jar tlc2.TLC -lncheck final -workers "$WORKERS" "$TLA_FILE"
