# Profiling an Application on Android
How to profile a test application for Android

## Set up Your Local Environment
1. Download ADB SDK
1. Add the SDK platform-tools to your PATH
   * export PATH="$PATH:$HOME/Library/Android/sdk/platform-tools"

## Run the ADB profiler
The ADB profiler is a custom script which provides
  * Battery statistics - battery.csv
  * Memory statistics - memory.csv
  * CPU statistics - cpu.json
`python buildscripts/mobile/adb_monitor.py`


## Enable USB Debugging on Android Device
Enabling USB debugging can differ by device, see https://developer.android.com/studio/debug/dev-options
1. Enable USB debugging via ADB, example
  * Select Settings/About phone(or tablet)
  * Select Build number 7 times, to enable Developer Options
  * Select Settings/Developer Options/USB Debugging
1. Connect the Android device to the computer via USB cable
1. Select "Aways allow from this computer" and OK, when the prompt "Allow USB debugging?" appears on the device

## Run the ADB Profiler Wirelessly
1. Ensure the local computer and Android device are on the same network
1. Connect the Android device to the computer via USB cable
1. Set the Android device's ADB listening port
   * `adb devices`
   * `adb tcpip 5555`
1. Disconnect the USB cable
1. Identify the Android's IP address
   * Settings/About phone(or tablet)/Status
   * `adb_ip=<ip_address>`, i.e., adb_ip=10.4.123.244
1. Connect wirelessly to the Android device
   * `adb connect $adb_ip`
1. Ensure you can connect to the Android device
   * `adb shell uptime`
1. Run the ADB profiler as detailed above
