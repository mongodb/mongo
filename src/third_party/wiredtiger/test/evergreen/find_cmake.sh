#!/bin/sh
set -o errexit  # Exit the script with error if any of the commands fail

# CMake version we fallback to and download when cmake doesn't exist on the
# host system.
CMAKE_MAJOR_VER=3
CMAKE_MINOR_VER=13
CMAKE_PATCH_VER=0
CMAKE_VERSION=$CMAKE_MAJOR_VER.$CMAKE_MINOR_VER.$CMAKE_PATCH_VER

# Adapted 'find_cmake' from mongo-c-driver evergreen infrastructure:
#   https://github.com/mongodb/mongo-c-driver/blob/master/.evergreen/find-cmake.sh
find_cmake ()
{
    if [ -n "$CMAKE" ]; then
        return 0
    elif [ -f "/opt/mongodbtoolchain/v4/bin/cmake" ]; then
        CMAKE="/opt/mongodbtoolchain/v4/bin/cmake"
        CTEST="/opt/mongodbtoolchain/v4/bin/ctest"
    elif [ -f "/Applications/CMake.app/Contents/bin/cmake" ]; then
        CMAKE="/Applications/CMake.app/Contents/bin/cmake"
        CTEST="/Applications/CMake.app/Contents/bin/ctest"
    elif [ -f "/opt/cmake/bin/cmake" ]; then
        CMAKE="/opt/cmake/bin/cmake"
        CTEST="/opt/cmake/bin/ctest"
    # Newer package system can be kept separate from the older "cmake".
    elif [ -f "/usr/bin/cmake3" ]; then
        CMAKE=/usr/bin/cmake3
        CTEST=/usr/bin/ctest3
    elif command -v cmake 2>/dev/null; then
        CMAKE=cmake
        CTEST=ctest
    elif uname -a | grep -iq 'x86_64 GNU/Linux'; then
        if [ -f "$(pwd)/cmake-$CMAKE_VERSION/bin/cmake" ]; then
            CMAKE="$(pwd)/cmake-$CMAKE_VERSION/bin/cmake"
            CTEST="$(pwd)/cmake-$CMAKE_VERSION/bin/ctest"
            return 0
        fi
        curl --retry 5 https://cmake.org/files/v$CMAKE_MAJOR_VER.$CMAKE_MINOR_VER/cmake-$CMAKE_VERSION-Linux-x86_64.tar.gz -sS --max-time 120 --fail --output cmake.tar.gz
        mkdir cmake-$CMAKE_VERSION
        tar xzf cmake.tar.gz -C cmake-$CMAKE_VERSION --strip-components=1
        CMAKE=$(pwd)/cmake-$CMAKE_VERSION/bin/cmake
        CTEST=$(pwd)/cmake-$CMAKE_VERSION/bin/ctest
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
        if [ -d  cmake-$CMAKE_VERSION ]; then rm -r cmake-$CMAKE_VERSION; fi
        curl --retry 5 https://cmake.org/files/v$CMAKE_MAJOR_VER.$CMAKE_MINOR_VER/cmake-$CMAKE_VERSION.tar.gz -sS --max-time 120 --fail --output cmake.tar.gz
        tar xzf cmake.tar.gz
        cd cmake-$CMAKE_VERSION
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
