#!/bin/sh

if [ "$#" -lt "5" ]; then
    echo "usage:"
    echo "$0 <android-sdk-path> <sysarch> <image-api-version> <directory> <test-path-in-directory>"
    exit 1
fi

set -o verbose
set -o errexit

ANDROID_SDK=$1
shift
ANDROID_SYSTEM_IMAGE_ARCH=$1
shift
ANDROID_IMAGE_API_VERSION=$1
shift
DIRECTORY=$1
shift
TEST_PATH_IN_DIRECTORY=$1
shift

EMULATOR_PID=''
cleanup() {
    echo "Cleanup handler invoked"

    if [ -z "$EMULATOR_PID" ]; then
        echo "No EMULATOR_PID found; not killing"
    else
        echo "Killing emulator"
        kill $EMULATOR_PID || true

        echo "Waiting for emulator to shut down"
        wait $EMULATOR_PID || true
    fi

    echo "Deleting the virtual device"
    $ANDROID_SDK/tools/bin/avdmanager delete avd -n android_avd || true

    echo "Exiting with status $1"
    exit $1
}

echo "Creating Android virtual device"
echo no | $ANDROID_SDK/tools/bin/avdmanager create avd --force -k "system-images;android-$ANDROID_IMAGE_API_VERSION;google_apis;$ANDROID_SYSTEM_IMAGE_ARCH" --name android_avd --abi google_apis/$ANDROID_SYSTEM_IMAGE_ARCH -p android_avd

trap 'cleanup $?' INT TERM EXIT

echo "Starting the virtual device on the emulator"
$ANDROID_SDK/emulator/emulator @android_avd -no-window -no-audio -no-accel &
EMULATOR_PID=$!

echo "Waiting for the adb service to be ready for commands"
$ANDROID_SDK/platform-tools/adb wait-for-device

echo "Making the adb service become root"
$ANDROID_SDK/platform-tools/adb root

echo "Copying the test to the virtual device"
$ANDROID_SDK/platform-tools/adb push $DIRECTORY /data

echo "Running the test on the virtual device"
$ANDROID_SDK/platform-tools/adb shell /data/$(basename $DIRECTORY)/$TEST_PATH_IN_DIRECTORY "$@"

# Do not add additional statements after the above adb invocation without
# forwarding its exit status or you will cause failing tests to appear
# to succeed.
