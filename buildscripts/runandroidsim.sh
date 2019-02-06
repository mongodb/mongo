#!/bin/bash

if [ "$#" -lt "5" ]; then
    echo "usage:"
    echo "$0 <android-sdk-path> <sysarch> <image-api-version> <directory> <test-path-in-directory>"
    exit 1
fi

set -o verbose
set -o errexit
set -o pipefail

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
$ANDROID_SDK/platform-tools/adb shell /data/$(basename $DIRECTORY)/$TEST_PATH_IN_DIRECTORY "$@" | tee android_sim_test_output.txt 2>&1

# On the android sim ( possibly on nomral android as well ) if a program fails its runtime link,
# for example because of a missing library, it will have an exit code of 0. In which case the
# android_sim_test_output.txt file will not contian the test output, but instead will contain
# "CANNOT LINK EXECUTABLE"
# So, once we're here in this script, the previous adb shell test command has either run
# successfully or failed to link. If it ran with errors, this script would have returned already
# because of the errexit.
# We test the output file to disambiguate
grep -q 'SUCCESS - All tests in all suites passed' android_sim_test_output.txt

# Do not add additional statements after the above command invocation without
# forwarding its exit status or you will cause failing tests to appear
# to succeed.
