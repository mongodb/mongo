Building MongoDB
================

Please note that prebuilt binaries are available on
[mongodb.org](http://www.mongodb.org/downloads) and may be the easiest
way to get started, rather than building from source.

To build MongoDB, you will need:

* A modern C++ compiler capable of compiling C++17. One of the following is required:
    * GCC 8.2 or newer
    * Clang 7.0 (or Apple XCode 10.2 Clang) or newer
    * Visual Studio 2019 version 16.4 or newer (See Windows section below for details)
* On Linux and macOS, the libcurl library and header is required. MacOS includes libcurl.
    * Fedora/RHEL - `dnf install libcurl-devel`
    * Ubuntu/Debian - `libcurl-dev` is provided by three packages. Install one of them:
      * `libcurl4-openssl-dev`
      * `libcurl4-nss-dev`
      * `libcurl4-gnutls-dev`
    * On Ubuntu, the lzma library is required. Install `liblzma-dev`
    * On Amazon Linux, the xz-devel library is required. `yum install xz-devel`
* Python 3.7.x and Pip modules:
  * See the section "Python Prerequisites" below.
* About 13 GB of free disk space for the core binaries (`mongod`,
  `mongos`, and `mongo`) and about 600 GB for the install-all target.

MongoDB supports the following architectures: arm64, ppc64le, s390x,
and x86-64.  More detailed platform instructions can be found below.


MongoDB Tools
--------------

The MongoDB command line tools (`mongodump`, `mongorestore`,
`mongoimport`, `mongoexport`, etc) have been rewritten in
[Go](http://golang.org/) and are no longer included in this
repository.

The source for the tools is now available at
[mongodb/mongo-tools](https://github.com/mongodb/mongo-tools).


Python Prerequisites
---------------

In order to build MongoDB, Python 3.7+ is required, and several Python
modules must be installed. Python 3 is included in macOS 10.15 and later.
For earlier macOS versions, Python 3 can be installed using Homebrew or
MacPorts or similar.

To install the required Python modules, run:

    $ python3 -m pip install -r etc/pip/compile-requirements.txt

Installing the requirements inside a python3 based virtualenv
dedicated to building MongoDB is recommended.

Note: In order to compile C-based Python modules, you'll also need the
Python and OpenSSL C headers. Run:

* Fedora/RHEL - `dnf install python3-devel openssl-devel`
* Ubuntu (20.04 and newer)/Debian (Bullseye and newer) - `apt install python-dev-is-python3 libssl-dev`
* Ubuntu (18.04 and older)/Debian (Buster and older) - `apt install python3.7-dev libssl-dev`


SCons
---------------

If you only want to build the database server `mongod`:

    $ python3 buildscripts/scons.py install-mongod

***Note***: For C++ compilers that are newer than the supported
version, the compiler may issue new warnings that cause MongoDB to
fail to build since the build system treats compiler warnings as
errors. To ignore the warnings, pass the switch
`--disable-warnings-as-errors` to scons.

    $ python3 buildscripts/scons.py install-mongod --disable-warnings-as-errors

To install `mongod` directly to `/opt/mongo`

    $ python3 buildscripts/scons.py DESTDIR=/opt/mongo install-mongod

To create an installation tree of the servers in `/tmp/unpriv` that
can later be copied to `/usr/priv`

    $ python3 buildscripts/scons.py DESTDIR=/tmp/unpriv PREFIX=/usr/priv install-servers

If you want to build absolutely everything (`mongod`, `mongo`, unit
tests, etc):

    $ python3 buildscripts/scons.py install-all-meta


SCons Targets
--------------

The following targets can be named on the scons command line to build and
install a subset of components:

* `install-mongod`
* `install-mongos`
* `install-core` (includes *only* `mongod` and `mongos`)
* `install-servers` (includes all server components)
* `install-devcore` (includes `mongod`, `mongos`, and `jstestshell` (formerly `mongo` shell))
* `install-all` (includes a complete end-user distribution and tests)
* `install-all-meta` (absolutely everything that can be built and installed)

***NOTE***: The `install-core` and `install-servers` targets are *not*
guaranteed to be identical. The `install-core` target will only ever include a
minimal set of "core" server components, while `install-servers` is intended
for a functional end-user installation. If you are testing, you should use the
`install-core` or `install-devcore` targets instead.

Where to find Binaries
----------------------

The build system will produce an installation tree into
`$DESTDIR/$PREFIX`. `DESTDIR` by default is `build/install` while
`PREFIX` is by default empty. This means that with all of the listed
targets all built binaries will be in `build/install/bin` by default.


Windows
--------------

See [the windows build
manual](https://github.com/mongodb/mongo/wiki/Build-Mongodb-From-Source#windows-specific-instructions)

Build requirements:
* Visual Studio 2017 version 15.9 or newer
* Python 3.7

Or download a prebuilt binary for Windows at www.mongodb.org.


Debian/Ubuntu
--------------

To install dependencies on Debian or Ubuntu systems:

    # apt-get install build-essential


OS X
--------------

Install Xcode 10.2 or newer.

FreeBSD
--------------

Install the following ports:

  * `devel/libexecinfo`
  * `lang/llvm70`
  * `lang/python`

Add `CC=clang70 CXX=clang++70` to the `scons` options, when building.


OpenBSD
--------------
Install the following ports:

  * `devel/libexecinfo`
  * `lang/gcc`
  * `lang/python`
