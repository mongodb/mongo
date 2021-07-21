#!/bin/sh
set -o errexit  # Exit the script with error if any of the commands fail

# Adapted 'find_cmake' from mongo-c-driver evergreen infrastructure:
#   https://github.com/mongodb/mongo-c-driver/blob/master/.evergreen/find-cmake.sh
find_cmake ()
{
    if [ ! -z "$CMAKE" ]; then
        return 0
    elif [ -f "/Applications/CMake.app/Contents/bin/cmake" ]; then
        CMAKE="/Applications/CMake.app/Contents/bin/cmake"
        CTEST="/Applications/CMake.app/Contents/bin/ctest"
    elif [ -f "/opt/cmake/bin/cmake" ]; then
        CMAKE="/opt/cmake/bin/cmake"
        CTEST="/opt/cmake/bin/ctest"
    elif command -v cmake 2>/dev/null; then
        CMAKE=cmake
        CTEST=ctest
    elif uname -a | grep -iq 'x86_64 GNU/Linux'; then
        if [ -f "$(pwd)/cmake-3.11.0/bin/cmake" ]; then
            CMAKE="$(pwd)/cmake-3.11.0/bin/cmake"
            CTEST="$(pwd)/cmake-3.11.0/bin/ctest"
            return 0
        fi
        curl --retry 5 https://cmake.org/files/v3.11/cmake-3.11.0-Linux-x86_64.tar.gz -sS --max-time 120 --fail --output cmake.tar.gz
        mkdir cmake-3.11.0
        tar xzf cmake.tar.gz -C cmake-3.11.0 --strip-components=1
        CMAKE=$(pwd)/cmake-3.11.0/bin/cmake
        CTEST=$(pwd)/cmake-3.11.0/bin/ctest
    elif [ -f "/cygdrive/c/cmake/bin/cmake" ]; then
        CMAKE="/cygdrive/c/cmake/bin/cmake"
        CTEST="/cygdrive/c/cmake/bin/ctest"
    elif [ -f "$(readlink -f cmake-install)"/bin/cmake ]; then
        # If we have a custom cmake install from an earlier build step.
        CMAKE="$(readlink -f cmake-install)/bin/cmake"
        CTEST="$(readlink -f cmake-install)/bin/ctest"
    fi

    if [ -z "$CMAKE" -o -z "$( $CMAKE --version 2>/dev/null )" ]; then
        # Some images have no cmake yet, or a broken cmake (see: BUILD-8570)
        echo "-- MAKE CMAKE --"
        CMAKE_INSTALL_DIR=$(readlink -f cmake-install)
        curl --retry 5 https://cmake.org/files/v3.11/cmake-3.11.0.tar.gz -sS --max-time 120 --fail --output cmake.tar.gz
        tar xzf cmake.tar.gz
        cd cmake-3.11.0
        ./bootstrap --prefix="${CMAKE_INSTALL_DIR}"
        make -j8
        make install
        cd ..
        CMAKE="${CMAKE_INSTALL_DIR}/bin/cmake"
        CTEST="${CMAKE_INSTALL_DIR}/bin/ctest"
        echo "-- DONE MAKING CMAKE --"
    fi

    echo "=========================================================="
    echo "CMake and CTest environment variables, paths and versions:"
    echo "CMAKE: ${CMAKE}"
    echo "CTEST: ${CTEST}"
    command -v ${CMAKE}
    command -v ${CTEST}
    ${CMAKE} --version
    ${CTEST} --version
    echo "=========================================================="
}

find_cmake
