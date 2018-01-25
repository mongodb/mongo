#!/bin/sh

if [ "$#" != "2" ]; then
    echo "usage:"
    echo "$0 <android-sdk-path> <test>"
    exit 1
fi

set -o verbose
set -o errexit

ANDROID_SDK=$1
TEST_PATH=$2
TEST_FILE=`basename $TEST_PATH`

EMULATOR_PID=''
cleanup() {
    kill $EMULATOR_PID
    wait $EMULATOR_PID
    $ANDROID_SDK/tools/bin/avdmanager delete avd -n android_avd
}

trap cleanup EXIT

# create a virtual device
echo no | $ANDROID_SDK/tools/bin/avdmanager create avd --force -k 'system-images;android-24;google_apis;arm64-v8a' --name android_avd --abi google_apis/arm64-v8a -p android_avd

# start the device on the emulator
$ANDROID_SDK/emulator/emulator @android_avd -no-window -no-audio &
EMULATOR_PID=$!

#wait for the adb service to be ready for commands
$ANDROID_SDK/platform-tools/adb wait-for-device

#have the adb service become root
$ANDROID_SDK/platform-tools/adb root

#move the test to the device
$ANDROID_SDK/platform-tools/adb push $TEST_PATH /data

#run the device
$ANDROID_SDK/platform-tools/adb shell /data/$TEST_FILE --tempPath /data
