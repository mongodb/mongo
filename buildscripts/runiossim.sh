#!/bin/sh

set -o verbose
set -o errexit

# This script used to create simulators called 'mongo-sim', but then
# failed to delete them leading to stale instances on the machines. We
# have since changed the name, and made the script smarter, but this
# startup code is here to clean out any old stale devices. It can
# probably be removed in a few weeks.
xcrun simctl list | grep mongo-sim | awk '{print $1}' | xargs xcrun simctl delete || true

if [ "$#" -lt "3" ]; then
    echo "usage:"
    echo "$0 <device> <runtime> <test>"
    exit 1
fi

DEVICE="$1"
shift
RUNTIME="$1"
shift
TEST="$1"
shift

cleanup() {
    echo "Shutting down simulator"
    xcrun simctl shutdown $_SimId || true

    echo "Erasing simulator"
    xcrun simctl erase $_SimId || true

    echo "Deleting simulator"
    xcrun simctl delete $_SimId || true

    echo "Exiting with status $1"
    exit $1
}

echo "Creating simulator"
_SimId=$(xcrun simctl create mongodb-simulator-$DEVICE.$RUNTIME "com.apple.CoreSimulator.SimDeviceType.$DEVICE" "com.apple.CoreSimulator.SimRuntime.$RUNTIME")
echo "Simulator created with ID $_SimId"

trap 'cleanup $?' INT TERM EXIT

echo "Booting simulator"
xcrun simctl boot $_SimId

echo "Spawning test program in simulator"
xcrun simctl spawn $_SimId "$TEST" "$@"

# Do not add additional statements after the above spawn without
# forwarding its exit status or you will cause failing tests to appear
# to succeed.
