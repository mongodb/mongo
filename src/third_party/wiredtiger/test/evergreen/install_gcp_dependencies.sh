#!/bin/sh
set -o errexit

# Install dependencies for the GCP SDK. This method is being used instead of having them installed 
# on system to reduce any versioning problems in the future if the GCP SDK requirements were to
# change. The installation of the libraries were followed from GCP SDK's documentation:
# https://github.com/googleapis/google-cloud-cpp/blob/main/doc/packaging.md
#
install_abseil ()
{
    mkdir abseil-cpp && cd abseil-cpp
    curl -sSL https://github.com/abseil/abseil-cpp/archive/20230125.0.tar.gz -o abseil-cpp.tar.gz
    tar --strip-components=1 -xzf abseil-cpp.tar.gz
    mkdir cmake-out && cd cmake-out
    $CMAKE \
        -DCMAKE_BUILD_TYPE=Release \
        -DABSL_BUILD_TESTING=OFF \
        -DBUILD_SHARED_LIBS=yes ../.
    make -j $(nproc)
    cd ..
    sudo $CMAKE --build cmake-out --target install
    cd ..
}

install_nlohmann_json ()
{
    mkdir json && cd json
    curl -sSL https://github.com/nlohmann/json/archive/v3.11.2.tar.gz -o nlohmann-json.tar.gz
    tar --strip-components=1 -xzf nlohmann-json.tar.gz
    mkdir cmake-out && cd cmake-out
    $CMAKE \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=yes \
        -DBUILD_TESTING=OFF \
        -DJSON_BuildTests=OFF ../.
    make -j $(nproc)
    cd ..
    sudo $CMAKE --build cmake-out --target install
    cd ..
}

install_crc32c ()
{
    mkdir crc32c && cd crc32c
    curl -sSL https://github.com/google/crc32c/archive/1.1.2.tar.gz -o crc32c.tar.gz
    tar --strip-components=1  -xzf crc32c.tar.gz
    mkdir cmake-out && cd cmake-out
    $CMAKE -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=yes \
        -DCRC32C_BUILD_TESTS=OFF \
        -DCRC32C_BUILD_BENCHMARKS=OFF \
        -DCRC32C_USE_GLOG=OFF ../.
    make -j $(nproc)
    cd ..
    sudo $CMAKE --build cmake-out --target install
    cd ..
}

if [ "$#" -ne 1 ]; then
    echo "Usage: install_gcp_dependencies.sh <cmake_path>"
    exit 1
fi

CMAKE=$1
install_abseil
install_nlohmann_json
install_crc32c
