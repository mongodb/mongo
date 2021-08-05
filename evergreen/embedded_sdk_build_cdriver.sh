DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

VERSION=${version}
WORKDIR=${workdir}

# build in a different directory then we run tests so that we can verify that the linking
# of tests are not relying any built in absolute paths
FINAL_PREFIX=$WORKDIR/src/build/mongo-embedded-sdk-$VERSION
BUILD_PREFIX=$FINAL_PREFIX-tmp

rm -rf mongo-c-driver

# NOTE: If you change the C Driver version here, also change the substitution in the CocoaPod podspec below in the apple builder.
git clone --branch r1.13 --depth 1 https://github.com/mongodb/mongo-c-driver.git
cd mongo-c-driver

# Fixup VERSION so we don't end up with -dev on it. Remove this once we are building a stable version and CDRIVER-2861 is resolved.
cp -f VERSION_RELEASED VERSION_CURRENT

trap "cat CMakeFiles/CMakeOutput.log" EXIT
export ${compile_env}
eval ${cmake_path} -DCMAKE_INSTALL_PREFIX=$BUILD_PREFIX -DENABLE_SHM_COUNTERS=OFF -DENABLE_SNAPPY=OFF -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_STATIC=OFF -DCMAKE_OSX_DEPLOYMENT_TARGET=${cdriver_cmake_osx_deployment_target} ${cdriver_cmake_flags}
trap - EXIT # cancel the previous trap '...' EXIT
make install VERBOSE=1

# TODO: Remove this when we upgrade to a version of the C driver that has CDRIVER-2854 fixed.
mkdir -p $BUILD_PREFIX/share/doc/mongo-c-driver
cp COPYING $BUILD_PREFIX/share/doc/mongo-c-driver
cp THIRD_PARTY_NOTICES $BUILD_PREFIX/share/doc/mongo-c-driver

mv $BUILD_PREFIX $FINAL_PREFIX
