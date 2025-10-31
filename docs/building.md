# Building MongoDB

Please note that prebuilt binaries are available on
[mongodb.org](http://www.mongodb.org/downloads) and may be the easiest
way to get started, rather than building from source.

To build MongoDB, you will need:

- A modern C++ compiler capable of compiling C++20. One of the following is required:
  - GCC 14.2
  - Clang 19.1
  - Apple XCode 16.4
  - Visual Studio 2022 version 17.0
- On Linux and macOS, the libcurl library and header is required. MacOS includes libcurl.
  - Fedora/RHEL - `dnf install libcurl-devel`
  - Ubuntu/Debian - `libcurl-dev` is provided by three packages. Install one of them:
    - `libcurl4-openssl-dev`
    - `libcurl4-nss-dev`
    - `libcurl4-gnutls-dev`
  - On Ubuntu, the lzma library is required. Install `liblzma-dev`
  - On Amazon Linux, the xz-devel library is required. `yum install xz-devel`
- Python 3.10
- About 13 GB of free disk space for the core binaries (`mongod`,
  `mongos`, and `mongo`).

If using a newer version of a C++ compiler than listed above, it may work. However the versions listed above have been verified to work.

MongoDB supports the following architectures: arm64, ppc64le, s390x,
and x86-64. More detailed platform instructions can be found below.

## Quick (re)Start

### Linux

```bash
python buildscripts/install_bazel.py
export PATH=~/.local/bin:$PATH
bazel build install-dist-test
bazel-bin/install/bin/mongod --version
```

## Bazel

If you only want to build the database server `mongod`:

    $ bazel build install-mongod

**_Note_**: For C++ compilers that are newer than the supported
version, the compiler may issue new warnings that cause MongoDB to
fail to build since the build system treats compiler warnings as
errors. To ignore the warnings, pass the switch
`--disable_warnings_as_errors=True` to the bazel command.

    $ bazel build install-mongod --disable_warnings_as_errors=True

If you want to build absolutely everything (`mongod`, `mongo`, unit
tests, etc):

    $ bazel build --build_tag_filters=mongo_binary //src/mongo/...

## Bazel Targets

The following targets can be named on the bazel command line to build and
install a subset of components:

- `install-mongod`
- `install-mongos`
- `install-core` (includes _only_ `mongod` and `mongos`)
- `install-dist` (includes all server components)
- `install-devcore` (includes `mongod`, `mongos`, and `jstestshell` (formerly `mongo` shell))

**_NOTE_**: The `install-core` and `install-dist` targets are _not_
guaranteed to be identical. The `install-core` target will only ever include a
minimal set of "core" server components, while `install-dist` is intended
for a functional end-user installation. If you are testing, you should use the
`install-devcore` or `install-dist` targets instead.

## Where to find Binaries

The build system will produce an installation tree into `bazel-bin/install`, as well
individual install target trees like `bazel-bin/<install-target>`.

## Windows

Build requirements:

- Visual Studio 2022 version 17.0 or newer
- Python 3.10

Or download a prebuilt binary for Windows at www.mongodb.org.

## Debian/Ubuntu

To install dependencies on Debian or Ubuntu systems:

    # apt-get install build-essential

## OS X

Install Xcode 16.4 or newer. Make sure macOS 15.5 platform
is installed.

Install llvm and lld, version 19 from brew:
brew install llvm@19 lld@19
