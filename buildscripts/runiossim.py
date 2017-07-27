#!/usr/bin/env python
"""
Script to create and boot an iOS simulator, run a program in the simulator, and clean up.
"""

from __future__ import absolute_import, print_function
import argparse
import subprocess

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run iOS Simulator")
    parser.add_argument("--test",
            required=True,
            type=str,
            help="Path to test and arguments to run on the simulator")
    args = parser.parse_args()

    print("Creating simulator")
    # use subprocess.check_call() for tasks because it throws an exception on failure
    subprocess.check_call(["xcrun", "simctl", "create", "mongo-sim", "com.apple.CoreSimulator.SimDeviceType.iPhone-7", "com.apple.CoreSimulator.SimRuntime.iOS-10-3"])
    try:
        print("Booting simulator")
        subprocess.check_call(["xcrun", "simctl", "boot", "mongo-sim"])

        print("Spawning test program in simulator")

        # split args["test"] by spaces to get array of arguments
        proc = subprocess.check_call(["xcrun", "simctl", "spawn", "mongo-sim"] + args.test.split())
        print(proc)
    finally:
        print("Shutting down simulator")
        # use subprocess.call() for shutdown tasks because they do not throw exceptions on failure
        subprocess.call(["xcrun", "simctl", "shutdown", "mongo-sim"])

        print("Erasing simulator")
        subprocess.call(["xcrun", "simctl", "erase", "mongo-sim"])
