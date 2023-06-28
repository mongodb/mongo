# WiredTiger's Azure Extension
## 1. Introduction
This extension allows WiredTiger storage source extensions to read from and write to objects stored
in Azure Blob Storage using WiredTiger’s provided internal abstraction for storing data in an object
storage service.

## 2. Building and running
This section describes how to build WiredTiger with the Azure extension enabled.

### Requirements
* CMake 3.13 or higher
* G++ 8.4 or higher
* clang version 6.0.0 or higher

### Building
There are two ways to build WiredTiger with the Azure extension:

1. Using a system installation of the Azure SDK.
2. Letting CMake manage the Azure SDK as an external project. This method will download the SDK, 
link to WiredTiger's build system, and build the extension.

There are two CMake flags associated with the Azure extension: `ENABLE_AZURE` and `IMPORT_AZURE_SDK`.
* `ENABLE_AZURE=1` is required to build the Azure extension.
* `IMPORT_AZURE_SDK={external,package}` is used to set the build method.
    *   `external` tells the compiler to download and install the SDK as part of the build.
    *   `package` tells the compiler to search for an existing system installation of the SDK.
    *    This flag should be set alongside the `ENABLE_AZURE` flag.
    *    If the `IMPORT_AZURE_SDK` flag is not specified, the compiler will assume a system
         installation of the SDK.

### Letting CMake manage the SDK dependency as an external project

This method configures CMake to download, compile, and install the Azure SDK while building the
Azure extension.

```bash
# Create a new directory to run the build from
mkdir build && cd build

# Configure and run cmake with Ninja
cmake -DENABLE_PYTHON=1 -DHAVE_UNITTEST=1 -DIMPORT_AZURE_SDK=external -DENABLE_AZURE=1 -G Ninja ../.
ninja
```

* The compiler flag `IMPORT_AZURE_SDK` must be set to `external` for this build method.
* `ENABLE_AZURE` will error out if the `IMPORT_AZURE_SDK` setting is not set.
## 3. Development
In order to run this extension after building, the developer must have an Azure connection string
locally to a container with the right permissions. The connection string must be stored in an
environment variable called `AZURE_STORAGE_CONNECTION_STRING`. To store the connection string
into an environment variable type
`export AZURE_STORAGE_CONNECTION_STRING="Azure connection string"` into the terminal.
## 4. Testing

### To run the tiered python tests for Azure:

```bash
# This will run all the tests in test_tiered19.py on the Azure storage source. The following
# command will run the tests from the build directory that was made earlier.
cd build
env WT_BUILDDIR=$(pwd) python3 ../test/suite/run.py -j 10 -v 4 test_tiered19
```

### To run the C unit tests for Azure:

```bash
# Once WiredTiger has been built with the Azure Extension, run the tests from the build directory
cd build/ext/storage_sources/azure_store/test/
./run_azure_unit_tests
```

To add any additional unit testing, add to the file `test_azure_connection.cpp`, alternatively if
the developer wishes to add a new test file, add it to the `SOURCES` list in
`create_test_executable()` (in `azure_store/test/CMakeLists.txt`).

## 5. Evergreen Testing
Currently the Evergreen testing runs both `test_tiered19.py` and the unit tests in
`test_azure_connection.cpp`. Should a developer wish to add additional tests to the extension, they
would first have to write the tests before adding it as a task to the evergreen.yml file.

Additionally, Evergreen has hidden the connection string for Azure and this is stored within the
Evergreen system.

When creating a new task, a developer should note that the CMake binary should be set to the MongoDB
V4 toolchain due to the CMake 3.13 minimum requirement.