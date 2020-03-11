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

echo "Downloading tla2tools.jar"
curl -LO tla.msr-inria.inria.fr/tlatoolbox/dist/tla2tools.jar
cd "$1"
java -XX:+UseParallelGC -cp ../tla2tools.jar tlc2.TLC -workers "$WORKERS" "$TLA_FILE"
