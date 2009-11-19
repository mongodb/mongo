
Building MongoDB
================

Scons
----------------

  For detail information about building, please see:    
  http://www.mongodb.org/display/DOCS/Building

  If you want to build everything (mongod, mongo, tools, etc):

     $ scons .

  If you only want to build the database:

     $ scons

  To install

     $ scons --prefix=/opt/mongo install

  Please note that prebuilt binaries are available on mongodb.org and may be the easier way to get started.

scons targets
-------------
* mongod
* mongos
* mongo
* mongoclient

*general notes
---------------
  COMPILER VERSIONS

  Mongo has been tested with GCC 4.x and Visual Studio 2008.  Older versions
  of GCC may not be happy.

windows
---------------

  See also http://www.mongodb.org/display/DOCS/Building+for+Windows

  Build requirements:
    - vc++ express or visual studio
    - python 2.5 (for scons - 2.6 might be needed for some regression tests)
    - scons
    - boost 1.35 (or higher)
    - windows sdk - tested with v6.0 v6.0a

  Or download a prebuilt binary for Windows at www.mongodb.org.

ubuntu
--------------

  scons libboost-dev libpcre++-dev xulrunner-1.9.1-dev

FreeBSD

  Install the following ports:

    - devel/boost
    - devel/libexecinfo
    - devel/pcre
    - lang/spidermonkey
