Building MongoDB
================

To build MongoDB, you will need:

* A modern C++ compiler. One of the following is required.
    * GCC 5.4.0 or newer
    * Clang 3.8 (or Apple XCode 8.3.2 Clang) or newer
    * Visual Studio 2015 Update 3 or newer (See Windows section below for details)
* Python 2.7.x and Pip modules:
  * pyyaml
  * typing

MongoDB supports the following architectures: arm64, ppc64le, s390x, and x86-64.
More detailed platform instructions can be found below.


MongoDB Tools
--------------

The MongoDB command line tools (mongodump, mongorestore, mongoimport, mongoexport, etc)
have been rewritten in [Go](http://golang.org/) and are no longer included in this repository.

The source for the tools is now available at [mongodb/mongo-tools](https://github.com/mongodb/mongo-tools).

Python Prerequisites
---------------

In order to build MongoDB, Python 2.7.x is required, and several Python modules. To install
the required Python modules, run:

    $ pip2 install -r buildscripts/requirements.txt

Note: If the `pip2` command is not available, `pip` without a suffix may be the pip command
associated with Python 2.7.x.

SCons
---------------

For detail information about building, please see [the build manual](https://github.com/mongodb/mongo/wiki/Build-Mongodb-From-Source)

If you want to build everything (mongod, mongo, tests, etc):

    $ python2 buildscripts/scons.py all

If you only want to build the database:

    $ python2 buildscripts/scons.py scons

To install

    $ python2 buildscripts/scons.py --prefix=/opt/mongo install

Please note that prebuilt binaries are available on [mongodb.org](http://www.mongodb.org/downloads) and may be the easiest way to get started.

SCons Targets
--------------

* mongod
* mongos
* mongo
* core (includes mongod, mongos, mongo)
* all

Windows
--------------

See [the windows build manual](https://github.com/mongodb/mongo/wiki/Build-Mongodb-From-Source#windows-specific-instructions)

Build requirements:
* Visual Studio 2015 Update 2 or newer
* Python 2.7, ActiveState ActivePython 2.7.x Community Edition for Windows is recommended

If using VS 2015 Update 3, two hotfixes are required to build. For details, see:
* https://support.microsoft.com/en-us/help/3207317/visual-c-optimizer-fixes-for-visual-studio-2015-update-3
* https://support.microsoft.com/en-za/help/4020481/fix-link-exe-crashes-with-a-fatal-lnk1000-error-when-you-use-wholearch

Or download a prebuilt binary for Windows at www.mongodb.org.

Debian/Ubuntu
--------------

To install dependencies on Debian or Ubuntu systems:

    # aptitude install build-essential
    # aptitude install libboost-filesystem-dev libboost-program-options-dev libboost-system-dev libboost-thread-dev

To run tests as well, you will need PyMongo:

    # aptitude install python-pymongo

OS X
--------------

Using [Homebrew](http://brew.sh):

    $ brew install mongodb

Using [MacPorts](http://www.macports.org):

    $ sudo port install mongodb

FreeBSD
--------------

Install the following ports:

  * devel/libexecinfo
  * lang/clang38
  * lang/python

Optional Components if you want to use system libraries instead of the libraries included with MongoDB

  * archivers/snappy
  * lang/v8
  * devel/boost
  * devel/pcre

Add `CC=clang38 CXX=clang++38` to the `scons` options, when building.

OpenBSD
--------------
Install the following ports:

  * devel/libexecinfo
  * lang/gcc
  * lang/python

Special Build Notes
--------------
  * [open solaris on ec2](building.opensolaris.ec2.md)

