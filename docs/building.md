
Building MongoDB
================

SCONS
---------------

For detail information about building, please see [the wiki](http://www.mongodb.org/display/DOCS/Building).

If you want to build everything (mongod, mongo, tools, etc):

    $ scons .

If you only want to build the database:

    $ scons

To install

    $ scons --prefix=/opt/mongo install

Please note that prebuilt binaries are available on [mongodb.org](http://www.mongodb.org/downloads) and may be the easiest way to get started.

SCONS TARGETS
--------------

* mongod
* mongos
* mongo
* mongoclient
* all

COMPILER VERSIONS
--------------

Mongo has been tested with GCC 4.x and Visual Studio 2008 and 2010.  Older versions
of GCC may not be happy.

WINDOWS
--------------

See http://www.mongodb.org/display/DOCS/Building+for+Windows

Build requirements:
* vc++ express or visual studio
* python 2.5 (for scons - 2.6 might be needed for some regression tests)
* scons
* boost 1.35 (or higher)

Or download a prebuilt binary for Windows at www.mongodb.org.

UBUNTU
--------------

To install dependencies on Ubuntu systems:

    # aptitude install scons build-essential
    # aptitude install libboost-filesystem-dev libboost-program-options-dev libboost-system-dev libboost-thread-dev

To run tests as well, you will need PyMongo:

    # aptitude install python-pymongo

Then build as usual with `scons`:

    $ scons all


OS X
--------------

Try [Homebrew](http://mxcl.github.com/homebrew/):

    $ brew install mongodb


FREEBSD
--------------

Install the following ports:

  * devel/boost
  * devel/libexecinfo
  * devel/pcre
  * lang/spidermonkey


Special Build Notes
--------------
  * [debian etch on ec2](building.debian.etch.ec2.html)
  * [open solaris on ec2](building.opensolaris.ec2.html)

