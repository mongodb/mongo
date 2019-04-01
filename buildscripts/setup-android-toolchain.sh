#!/bin/sh

set -o verbose
set -o errexit

SDK_ROOT=$1
if [ -z $SDK_ROOT ]; then
    echo "usage: $0 <sdk-root>"
    exit 1
fi
shift

NDK=android-ndk-r19c
NDK_PACKAGE=$NDK-linux-x86_64.zip
# The releases of the sdk tools are published at
#   https://developer.android.com/studio/releases/sdk-tools
SDK_PACKAGE=sdk-tools-linux-4333796.zip # 26.1.1

test -e $SDK_PACKAGE || curl -O https://dl.google.com/android/repository/$SDK_PACKAGE
test -e $NDK_PACKAGE || curl -O https://dl.google.com/android/repository/$NDK_PACKAGE

if [ ! -e  $SDK_ROOT ]; then
    mkdir $SDK_ROOT
    (
        cd $SDK_ROOT
        unzip -q ../$SDK_PACKAGE
        yes | ./tools/bin/sdkmanager --channel=0 \
            "build-tools;28.0.0" \
            "emulator"  \
            "patcher;v4"  \
            "platforms;android-28"  \
            "platform-tools"  \
            "system-images;android-24;google_apis;arm64-v8a"  \
            "system-images;android-24;google_apis;armeabi-v7a"  \
            "system-images;android-24;google_apis;x86_64" \
        | grep -v Unzipping
    )
    unzip -q $NDK_PACKAGE
    mv $NDK $SDK_ROOT/ndk-bundle
fi
