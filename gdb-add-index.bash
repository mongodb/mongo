#!/bin/bash

# Default build type
BUILD_TYPE="RelWithDebInfo"

# Display usage information
usage() {
    echo "Usage: $0 [-t build_type]"
    echo "  -t  Specify build type (RelWithDebInfo, Debug, Release). Default is RelWithDebInfo."
    exit 0
}

# Parse command-line arguments
while getopts ":t:" opt; do
    case $opt in
        t)
            BUILD_TYPE="$OPTARG"
            ;;
        *)
            usage
            ;;
    esac
done

# Validate the build type
if [[ "$BUILD_TYPE" != "RelWithDebInfo" && "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" ]]; then
    echo "Error: Invalid build type. Must be one of: RelWithDebInfo, Debug, Release."
    usage
fi

# Define the build directory path
BUILD_DIR="build/$BUILD_TYPE"

# Check if the build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory '$BUILD_DIR' does not exist."
    exit 1
fi

# Find all .so files in the build directory
find "$BUILD_DIR" -type f -name "*.so" | while read -r lib; do
    echo "Processing $lib..."
    # Use gdb-add-index to generate an index for the shared library file
    gdb-add-index "$lib"
    if [ $? -eq 0 ]; then
        echo "Successfully added index to $lib"
    else
        echo "Failed to add index to $lib"
    fi
done