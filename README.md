<div align="center">
  <br />
  <p>
    <a href="https://www.mongodb.com/es"><img src="https://webassets.mongodb.com/_com_assets/cms/leaf-2g1s5txorx.svg" width="256" height="256" alt="Normal" /></a>
  </p>
 </div>

# MongoDB README 

Welcome to MongoDB!

## COMPONENTS

  ``mongod`` - The database server.
  ``mongos`` - Sharding router.
  ``mongo``  - The database shell (uses interactive javascript).

## UTILITIES

  ``install_compass``   - Installs MongoDB Compass for your platform.

## BUILDING

  See [docs/building.md](https://github.com/mongodb/mongo/blob/master/docs/building.md).

## RUNNING

  For command line options invoke:

    $ ./mongod --help

  To run a single server database:

    $ sudo mkdir -p /data/db
    $ ./mongod
    $
    $ # The mongo javascript shell connects to localhost and test database by default:
    $ ./mongo
    > help

## INSTALLING COMPASS

  You can install compass using the install_compass script packaged with MongoDB:

    $ ./install_compass

  This will download the appropriate MongoDB Compass package for your platform
  and install it.

## DRIVERS

  Client drivers for most programming languages are available at
  [https://docs.mongodb.com/manual/applications/drivers/](https://docs.mongodb.com/manual/applications/drivers/). Use the shell
  ("mongo") for administrative tasks.

## BUG REPORTS

  See [https://github.com/mongodb/mongo/wiki/Submit-Bug-Reports](https://github.com/mongodb/mongo/wiki/Submit-Bug-Reports).

## PACKAGING

  Packages are created dynamically by the package.py script located in the
  buildscripts directory. This will generate RPM and Debian packages.

## DOCUMENTATION

  [https://docs.mongodb.com/manual/](https://docs.mongodb.com/manual/)

## CLOUD HOSTED MONGODB

  [https://www.mongodb.com/cloud/atlas](https://www.mongodb.com/cloud/atlas)

## FORUMS

  [https://community.mongodb.com](https://community.mongodb.com)

    A forum for technical questions about using MongoDB.

  [https://community.mongodb.com/c/server-dev](https://community.mongodb.com/c/server-dev)

    A forum for technical questions about building and developing MongoDB.

## LEARN MONGODB

  [https://university.mongodb.com/](https://university.mongodb.com/)

## LICENSE
*The complete license is available in [LICENSE-Community.txt](https://github.com/mongodb/mongo/blob/master/LICENSE-Community.txt)*

  ````
  MongoDB is free and the source is available. Versions released prior to
  October 16, 2018 are published under the AGPL. All versions released after
  October 16, 2018, including patch fixes for prior versions, are published
  under the Server Side Public License (SSPL) v1. See individual files for
  details.
  ````
