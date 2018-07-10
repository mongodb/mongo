# Profiling an Application on iOS
How to profile a test application for iOS

## Set up Your Mac OSX Environment
1. Download Xcode

## Set up Xcode
1. Enable the Developer app on the iOS device
   * Settings/General/Device Management: Select the Developer app and Trust

## MongoProfiler
The `MongoProfiler` is a custom profiling template which includes
  * Energy Log - this must be imported from the iOS device and should be captured untethered
    Captures battery level
  * Activity Monitor - this can be captured via USB cable or wirelessly
    Captures CPU load
  * Virtual Memory Trace - this can be captured via USB cable or wirelessly
    Captures memory usage

## Run Profiler on the Mac
1. Connect the iOS device to the Mac via USB cable
1. Start Instruments on Mac
  * Can be started from within Xcode or from the Spotlight Search
1. Connect to the iOS device and select an app to profile
1. Open a Custom profiling template and select `MongoProfiler`
1. Start recording
1. Start the application on the iOS device
1. Stop recording in Instruments

## Run Profiler Wirelessly on the Mac
1. Ensure the Mac and iOS device are on the same WiFi network
1. Connect the iOS device to the Mac via USB cable
1. Pair the devices in Xcode
See https://help.apple.com/xcode/mac/9.0/index.html?localePath=en.lproj#/devbc48d1bad
  * Select Windows/Devices & Simulators
  * Select the Devices tab, select the iOS device and select `Connect via network`
1. Disconnect the USB cable
1. Follow the instructions from step 2 in `Run profiler on the Mac`

## Measuring Energy Usage on the iOS device
Unfortunately Instruments cannot measure Energy usage by recording from the Mac, but only from the iOS device.
1. Select Settings/Developer/Logging on the iOS device
1. Enable Energy
1. Select `Start Recording` (note this can be done before recording within Instruments for the other probes)
1. After the test is completed, select `Stop Recording`
1. From within Instruments on the Mac (note this can be done using the `MongoProfiler` so all probes are captured together)
  * Select File, `Import Logged Data From Device`
