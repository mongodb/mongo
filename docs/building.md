# Building MongoDB

Please note that prebuilt binaries are available on
[mongodb.org](http://www.mongodb.org/downloads) and may be the easiest
way to get started, rather than building from source.

To build MongoDB, you will need:

- A modern C++ compiler capable of compiling C++20. One of the following is required:
  - GCC 11.3 or newer
  - Clang 12.0 (or Apple XCode 13.0 Clang) or newer
  - Visual Studio 2022 version 17.0 or newer (See Windows section below for details)
- On Linux and macOS, the libcurl library and header is required. MacOS includes libcurl.
  - Fedora/RHEL - `dnf install libcurl-devel`
  - Ubuntu/Debian - `libcurl-dev` is provided by three packages. Install one of them:
    - `libcurl4-openssl-dev`
    - `libcurl4-nss-dev`
    - `libcurl4-gnutls-dev`
  - On Ubuntu, the lzma library is required. Install `liblzma-dev`
  - On Amazon Linux, the xz-devel library is required. `yum install xz-devel`
- Python 3.10.x and Pip modules:
  - See the section "Python Prerequisites" below.
- About 13 GB of free disk space for the core binaries (`mongod`,
  `mongos`, and `mongo`) and about 600 GB for the install-all target.

MongoDB supports the following architectures: arm64, ppc64le, s390x,
and x86-64. More detailed platform instructions can be found below.

## Quick (re)Start

### Linux Workstation

```bash
git clean -fdx # USE WITH CAUTION! Delete every untracked file including .gitignored files (this is basically everything)
# Close and reopen your terminal to clear out any problems from your old environment
python3 --version # Should be >=3.10. If it is not, follow [online instructions](https://www.python.org/downloads/) to install python.
python3 -m venv python3-venv --prompt mongo # Create a virtual environment. "python3-venv" is non standard but it is kept since it is assumed elsewhere in our code.
source python3-venv/bin/activate # You should see a (mongo) appear in your terminal
which python3 # This should point to the python in python3-venv

# It is also non standard to install poetry into its own virtual environment.
# However, the idea is to make even fewer unpinned dependencies.
# Install poetry 1.8.3 into the virtual env, then install all
# required python dependencies to build and test.
buildscripts/poetry_sync.sh

python3 buildscripts/scons.py --build-profile=opt
ninja -f opt.ninja -j 200 install-devcore
```

## Python Prerequisites

In order to build MongoDB, Python 3.10+ is required, and several Python
modules must be installed. Python 3 is included in macOS 10.15 and later.
For earlier macOS versions, Python 3 can be installed using Homebrew or
MacPorts or similar.

MongoDB manages our python dependencies with poetry.
You can see other install instructions for poetry by reading this [install guide](https://python-poetry.org/).

Installing the requirements inside a python3 based virtualenv
dedicated to building MongoDB is optional but recommended.

    $ python3 -m venv <venv_path> --prompt mongo # Optional (venv_path can be a path of your choice)
    $ source <venv_path>/bin/activate # Optional (might be slightly different based on your shell)
    $ buildscripts/poetry_sync.sh

Note: In order to compile C-based Python modules, you'll also need the
Python and OpenSSL C headers. Run:

- Fedora/RHEL - `dnf install python3-devel openssl-devel`
- Ubuntu (20.04 and newer)/Debian (Bullseye and newer) - `apt install python-dev-is-python3 libssl-dev`
- Ubuntu (18.04 and older)/Debian (Buster and older) - `apt install python3.7-dev libssl-dev`

Note: If you are running poetry manually and seeing errors involving "Prompt
dismissed.." you might need to run the following command before poetry install
(`buildscripts/poetry-sync.sh` does this internally).

    $ export PYTHON_KEYRING_BACKEND=keyring.backends.null.Keyring

## SCons

If you only want to build the database server `mongod`:

    $ python3 buildscripts/scons.py install-mongod

**_Note_**: For C++ compilers that are newer than the supported
version, the compiler may issue new warnings that cause MongoDB to
fail to build since the build system treats compiler warnings as
errors. To ignore the warnings, pass the switch
`--disable-warnings-as-errors` to scons.

    $ python3 buildscripts/scons.py install-mongod --disable-warnings-as-errors

**_Note_**: On memory-constrained systems, you may run into an error such as `g++: fatal error: Killed signal terminated program cc1plus`. To use less memory during building, pass the parameter `-j1` to scons. This can be incremented to `-j2`, `-j3`, and higher as appropriate to find the fastest working option on your system.

    $ python3 buildscripts/scons.py install-mongod -j1

To install `mongod` directly to `/opt/mongo`

    $ python3 buildscripts/scons.py DESTDIR=/opt/mongo install-mongod

To create an installation tree of the servers in `/tmp/unpriv` that
can later be copied to `/usr/priv`

    $ python3 buildscripts/scons.py DESTDIR=/tmp/unpriv PREFIX=/usr/priv install-servers

If you want to build absolutely everything (`mongod`, `mongo`, unit
tests, etc):

    $ python3 buildscripts/scons.py install-all-meta

## SCons Targets

The following targets can be named on the scons command line to build and
install a subset of components:

- `install-mongod`
- `install-mongos`
- `install-core` (includes _only_ `mongod` and `mongos`)
- `install-servers` (includes all server components)
- `install-devcore` (includes `mongod`, `mongos`, and `jstestshell` (formerly `mongo` shell))
- `install-all` (includes a complete end-user distribution and tests)
- `install-all-meta` (absolutely everything that can be built and installed)

**_NOTE_**: The `install-core` and `install-servers` targets are _not_
guaranteed to be identical. The `install-core` target will only ever include a
minimal set of "core" server components, while `install-servers` is intended
for a functional end-user installation. If you are testing, you should use the
`install-core` or `install-devcore` targets instead.

## Where to find Binaries

The build system will produce an installation tree into
`$DESTDIR/$PREFIX`. `DESTDIR` by default is `build/install` while
`PREFIX` is by default empty. This means that with all of the listed
targets all built binaries will be in `build/install/bin` by default.

## Windows

Build requirements:

- Visual Studio 2022 version 17.0 or newer
- Python 3.10

Or download a prebuilt binary for Windows at www.mongodb.org.

## Debian/Ubuntu

To install dependencies on Debian or Ubuntu systems:

    # apt-get install build-essential

## OS X

Install Xcode 13.0 or newer.

## FreeBSD

Install the following ports:

- `devel/libexecinfo`
- `lang/llvm70`
- `lang/python`

Add `CC=clang12 CXX=clang++12` to the `scons` options, when building.

## OpenBSD

Install the following ports:

- `devel/libexecinfo`
- `lang/gcc`
- `lang/python`
