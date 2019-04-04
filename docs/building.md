Building MerizoDB
================

To build MerizoDB, you will need:

* A modern C++ compiler capable of compiling C++17. One of the following is required:
    * GCC 8.0 or newer
    * Clang 7.0 (or Apple XCode 10.0 Clang) or newer
    * Visual Studio 2017 version 15.9 or newer (See Windows section below for details)
* On Linux and macOS, the libcurl library and header is required. MacOS includes libcurl.
    * Fedora/RHEL - `dnf install libcurl-devel`
    * Ubuntu/Debian - `apt-get install libcurl-dev`
* Python 2.7.x and Pip modules:
  * See the section "Python Prerequisites" below.

MerizoDB supports the following architectures: arm64, ppc64le, s390x, and x86-64.
More detailed platform instructions can be found below.


MerizoDB Tools
--------------

The MerizoDB command line tools (merizodump, merizorestore, merizoimport, merizoexport, etc)
have been rewritten in [Go](http://golang.org/) and are no longer included in this repository.

The source for the tools is now available at [merizodb/merizo-tools](https://github.com/merizodb/merizo-tools).

Python Prerequisites
---------------

In order to build MerizoDB, Python 2.7.x is required, and several Python modules. To install
the required Python modules, run:

    $ pip2 install -r etc/pip/compile-requirements.txt

Note: If the `pip2` command is not available, `pip` without a suffix may be the pip command
associated with Python 2.7.x.

SCons
---------------

For detail information about building, please see [the build manual](https://github.com/merizodb/merizo/wiki/Build-Merizodb-From-Source)

If you want to build everything (merizod, merizo, tests, etc):

    $ python2 buildscripts/scons.py all

If you only want to build the database:

    $ python2 buildscripts/scons.py merizod

***Note***: For C++ compilers that are newer than the supported version, the compiler may issue new warnings that cause MerizoDB to fail to build since the build system treats compiler warnings as errors. To ignore the warnings, pass the switch `--disable-warnings-as-errors` to scons.

    $ python2 buildscripts/scons.py merizod --disable-warnings-as-errors

To install

    $ python2 buildscripts/scons.py --prefix=/opt/merizo install

Please note that prebuilt binaries are available on [merizodb.org](http://www.merizodb.org/downloads) and may be the easiest way to get started.

SCons Targets
--------------

The following targets can be named on the scons command line to build only certain components:

* merizod
* merizos
* merizo
* core (includes merizod, merizos, merizo)
* all

Windows
--------------

See [the windows build manual](https://github.com/merizodb/merizo/wiki/Build-Merizodb-From-Source#windows-specific-instructions)

Build requirements:
* Visual Studio 2017 version 15.9 or newer
* Python 2.7, ActiveState ActivePython 2.7.x Community Edition for Windows is recommended

Or download a prebuilt binary for Windows at www.merizodb.org.

Debian/Ubuntu
--------------

To install dependencies on Debian or Ubuntu systems:

    # aptitude install build-essential
    # aptitude install libboost-filesystem-dev libboost-program-options-dev libboost-system-dev libboost-thread-dev

To run tests as well, you will need PyMongo:

    # aptitude install python-pymerizo

OS X
--------------

Using [Homebrew](http://brew.sh):

    $ brew install merizodb

Using [MacPorts](http://www.macports.org):

    $ sudo port install merizodb

FreeBSD
--------------

Install the following ports:

  * devel/libexecinfo
  * lang/llvm70
  * lang/python

Optional Components if you want to use system libraries instead of the libraries included with MerizoDB

  * archivers/snappy
  * devel/boost
  * devel/pcre

Add `CC=clang70 CXX=clang++70` to the `scons` options, when building.

OpenBSD
--------------
Install the following ports:

  * devel/libexecinfo
  * lang/gcc
  * lang/python
