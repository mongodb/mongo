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

## Run the ADB Profiler Wirelessly
1. Ensure the local computer and Android device are on the same network
1. Connect the Android device to the computer via USB cable
1. Set the Android device's ADB listening port
   * `adb devices`
   * `adb tcpip 5555`
1. Disconnect the USB cable
1. Identify the Android's IP address
   * Settings/About phone/Status
   * `adb_ip=<ip_address>`, i.e., adb_ip=10.4.123.244
1. Connect wirelessly to the Android device
   * `adb connect $adb_ip`
1. Ensure you can connect to the Android device
   * `adb shell uptime`
1. Run the ADB profiler as detailed above
