# WiredTiger's S3 Extension

## Building and running

This is a guide to build WiredTiger with the S3 extension enabled.

There are two different ways to build WiredTiger with S3 extension:
1. Using a system installation of the AWS SDK dependency.
2. Letting CMake manage the AWS SDK dependency as an external project, letting it download and compile each time.

There are two CMake flags associated with the S3 extension: `ENABLE_S3` and `IMPORT_S3_SDK`.
* `ENABLE_S3=1` is required to build the S3 extension.
* `IMPORT_S3_SDK={external,package}` is used to set the build method.
    *   `external` tells the compiler to download and install the SDK as part of the build.
    *   `package` tells the compiler to search for an existing system installation of the SDK.
    *    This flag should be set alongside the `ENABLE_S3` flag.
    *    If the `IMPORT_S3_SDK` flag is not specified, the compiler will assume a system installation of the SDK.

#### Instructions on installing AWS SDK for C++ locally

Follow the [guide to install SDK locally](https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/setup-linux.html):
* Note: The S3 extension uses the S3-CRT client provided by the SDK. This is different from the S3 client, so it is important to install the right dependency.
Set `BUILD_ONLY` flag to "s3-crt" to install only the necessary dependencies for this extension.


##### **First** build method - meeting AWS SDK dependency through system installed library

This method will find an existing system installation of the SDK and will not require a download stage during build.

```bash
# Create a new directory to run your build from
$ mkdir build && cd build

# Configure and run cmake with Ninja
cmake -DENABLE_PYTHON=1 -DHAVE_DIAGNOSTIC=1 -DHAVE_UNITTEST=1 -DENABLE_S3=1 -DENABLE_STRICT=0 -DCMAKE_CXX_FLAGS="-ggdb" -G Ninja ../.
ninja
```

* Setting the `IMPORT_S3_SDK` flag is optional for this build method.
* `ENABLE_S3` will default to looking for a local installation of the SDK.
* `CMAKE_CXX_FLAGS="-ggdb"` should be set to enable gdb for debugging information.
* `HAVE_UNITTEST` should be set to 1 so the Catch2 framework for S3 Unit tests is installed.
* `ENABLE_STRICT` should be set to 0.
    This is to to turn off strict compiler warnings so it does not pick up different formatting errors of the 3rd party dependencies.


##### **Second** build method - letting CMake manage the SDK dependency as an external project

This method configures CMake to download, compile, and install the AWS SDK while building the S3 extension.

```bash
# Create a new directory to run your build from
$ mkdir build && cd build

# Configure and run cmake with Ninja
cmake -DENABLE_PYTHON=1 -DHAVE_DIAGNOSTIC=1 -DHAVE_UNITTEST=1 -DIMPORT_S3_SDK=external -DENABLE_S3=1 -DENABLE_STRICT=0 -DCMAKE_CXX_FLAGS="-ggdb" -G Ninja ../.
ninja
```

* The compiler flag `IMPORT_S3_SDK` must be set to `external` for this build method.
* `ENABLE_S3` defaults to looking for a local version, the `IMPORT_S3_SDK` setting will override that default.


## Testing

#### To run the tiered python tests for S3:

```bash
# This will run all tiered tests on storage sources including S3, run the tests from the build directory
cd build
python3 ../test/suite/run.py -v 4 tiered
```

#### To run the C unit tests for S3

```bash
# Once WiredTiger has been built with the S3 Extension, run the tests from the build directory
cd build
ext/storage_sources/s3_store/test/run_s3_unit_tests 
```

To add any additional unit testing, add to the file `test_s3_connection.cpp`, alternatively if you
wish to add a new test file, add it to the `SOURCES` list in`create_test_executable()` 
(in `s3_store/test/CMakeLists.txt`). 
