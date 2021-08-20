# Building WiredTiger with CMake

### Build Dependencies

To build with CMake we **require** the following dependencies:

* `cmake` : Official CMake install instructions found here: https://cmake.org/install/
  * *WiredTiger supports CMake 3.10+*
* `ninja` : Official ninja install instructions found here: https://ninja-build.org/
* `python3-dev` : Consult your package management application for installation instructions. This is needed for building with Python support.
* `swig`: Consult your package management application for installation instructions. This is needed for building with Python support.

We also strongly suggest the following dependencies are also installed (for improved build times):

* `ccache` : Official ccache download instructions found here: https://ccache.dev/download.html

##### Package Manager Instructions

Alternatively you can use your system's package manager to install the dependencies listed above. Depending on the system, the following commands can be run:

###### Install commands for Ubuntu & Debian (tested on Ubuntu 18.04)

```bash
sudo apt-get install cmake cmake-curses-gui
sudo apt-get install ccache
sudo apt-get install ninja-build
sudo apt-get install python3-dev swig
```

###### Install commands for Mac (using HomeBrew)

```bash
brew install ninja
brew install ccache
brew install cmake
brew install python
brew install swig
```

###### Install commands for Windows (using Chocolatey)

```bash
choco install cmake
choco install ninja
choco install ccache --version=3.7.9
choco install swig
choco install python --pre
```

### Building the WiredTiger Library

> *The below commands are written for Linux and Darwin hosts. Windows instructions coming soon!*

Building the WiredTiger library is relatively straightforward. Navigate to the top level of the WiredTiger repository and run the following commands:

###### Configure your build

```bash
# Create a new directory to run your build from
$ mkdir build && cd build
# Run the cmake configure step. Note: '-G Ninja' tells CMake to generate a ninja build
$ cmake -G Ninja ../.
...
-- Configuring done
-- Generating done
-- Build files have been written to: /home/wiredtiger/build
```

*See [Configuration Options](#configuration-options) for additional configuration options.*

###### Run your build

In the same directory you configured your build, run the `ninja` command to start the build:

```bash
$ ninja
...
[211/211 (100%) 2.464s] Creating library symlink libwiredtiger.so
```

*Note: Ninja doesn't need a `-j` option; it knows how many cores are available.*

###### Configuration Options

There are a number of additional configuration options you can pass to the CMake configuration step. A summary of some important options you will come to know:

* `-DENABLE_STATIC=1` : Compile WiredTiger as a static library
* `-DENABLE_LZ4=1` : Build the lz4 compressor extension
* `-DENABLE_SNAPPY=1` : Build the snappy compressor extension
* `-DENABLE_ZLIB=1` : Build the zlib compressor extension
* `-DENABLE_ZSTD=1` : Build the libzstd compressor extension
* `-DENABLE_SODIUM=1` : Build the libsodium encryptor extension
* `-DHAVE_DIAGNOSTIC=1` : Enable WiredTiger diagnostics
* `-DHAVE_ATTACH=1` : Enable to pause for debugger attach on failure
* `-DENABLE_STRICT=1` : Compile with strict compiler warnings enabled
* `-DENABLE_PYTHON=1` : Compile the python API
* `-DCMAKE_INSTALL_PREFIX=<path-to-install-directory>` : Path to install directory

---

An example of using the above configuration options during the configuration step:

```bash
$ cmake -DENABLE_STATIC=1 -DENABLE_LZ4=1 -DENABLE_SNAPPY=1 -DENABLE_ZLIB=1 -DENABLE_ZSTD=1 -DHAVE_DIAGNOSTIC=1 -DHAVE_ATTACH=1 -DENABLE_STRICT=1 -G Ninja ../.
```

---

You can further look at all the available configuration options (and also dynamically change them!) by running `ccmake` in your build directory:

```bash
$ cd build
$ ccmake .
```

*The configuration options can also be viewed in `cmake/configs/base.cmake`*.

###### Switching between GCC and Clang (POSIX only)

By default CMake will use your default system compiler (`cc`). If you want to use a specific toolchain you can pass a toolchain file! We have provided a toolchain file for both GCC (`cmake/toolchains/gcc.cmake`) and Clang (`cmake/toolchains/clang.cmake`). To use either toolchain you can pass the `-DCMAKE_TOOLCHAIN_FILE=` to the CMake configuration step. For example:

*Using the GCC Toolchain*

```bash
$ cd build
$ cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/gcc.cmake -G Ninja ../.
```

*Using the Clang Toolchain*

```bash
$ cd build
$ cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/clang.cmake -G Ninja ../.
```

### Running WiredTiger C Tests

The WiredTiger CMake build makes available a suite of C/C++ based tests (separate from the Python testsuite). The run all the tests available, ensure you're in the build directory and execute:

```bash
ctest -j$(nproc)
```

To run with verbose output:

```bash
ctest -j$(nproc) -VV
```

To run a specfic test:

```bash
# Note: -R specifies a regex, where any matching test will be run
ctest -R <test_name> -j$(nproc)
```

See `ctest --help` for a range of additional supported options.
