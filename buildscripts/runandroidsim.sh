#!/bin/sh

if [ "$#" -lt "4" ]; then
    echo "usage:"
    echo "$0 <android-sdk-path> <sysarch> <directory> <test-path-in-directory>"
    exit 1
fi

set -o verbose
set -o errexit

ANDROID_SDK=$1
shift
ANDROID_SYSTEM_IMAGE_ARCH=$1
shift
DIRECTORY=$1
shift
TEST_PATH_IN_DIRECTORY=$1
shift

EMULATOR_PID=''
cleanup() {
    kill $EMULATOR_PID
    wait $EMULATOR_PID
    $ANDROID_SDK/tools/bin/avdmanager delete avd -n android_avd
}

trap cleanup EXIT

# create a virtual device
echo no | $ANDROID_SDK/tools/bin/avdmanager create avd --force -k "system-images;android-24;google_apis;$ANDROID_SYSTEM_IMAGE_ARCH" --name android_avd --abi google_apis/$ANDROID_SYSTEM_IMAGE_ARCH -p android_avd

# start the device on the emulator
$ANDROID_SDK/emulator/emulator @android_avd -no-window -no-audio &
EMULATOR_PID=$!

#wait for the adb service to be ready for commands
$ANDROID_SDK/platform-tools/adb wait-for-device

#have the adb service become root
$ANDROID_SDK/platform-tools/adb root

#move the test to the device
$ANDROID_SDK/platform-tools/adb push $DIRECTORY /data

#run the device
$ANDROID_SDK/platform-tools/adb shell /data/$(basename $DIRECTORY)/$TEST_PATH_IN_DIRECTORY "$@"
