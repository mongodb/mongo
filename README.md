![MongoDB Logo](https://webassets.mongodb.com/_com_assets/cms/MongoDB_Logo_FullColorBlack_RGB-4td3yuxzjs.png)

# MongoDB README
Welcome to MongoDB!

## COMPONENTS
- mongod - The database server.
- mongos - Sharding router.
- mongo  - The database shell (uses interactive javascript).

## UTILITIES
- install_compass   - Installs MongoDB Compass for your platform.

## BUILDING

  See [docs/building.md](docs/building.md).

## RUNNING
For command line options invoke:  

```$ ./mongod --help```

To run a single server database:
```
$ sudo mkdir -p /data/db
$ ./mongod
$
$ # The mongo javascript shell connects to localhost and test database by default:
$ ./mongo
> help
```

## INSTALLING COMPASS
You can install compass using the install_compass script packaged with MongoDB:

```$ ./install_compass```

This will download the appropriate MongoDB Compass package for your platform
and install it.

## DRIVERS
Client drivers for most programming languages are available at https://docs.mongodb.com/manual/applications/drivers/. Use the shell ("mongo") for administrative tasks.

## BUG REPORTS
See https://github.com/mongodb/mongo/wiki/Submit-Bug-Reports.

## PACKAGING
Packages are created dynamically by the [package.py](buildscripts/packager.py) script located in the buildscripts directory. This will generate RPM and Debian packages.

## DOCUMENTATION
https://docs.mongodb.com/manual/

## CLOUD HOSTED MONGODB
https://www.mongodb.com/cloud/atlas

## FORUMS
A [forum](https://community.mongodb.com) for technical questions about using MongoDB.

A [forum](https://community.mongodb.com/c/server-dev) for technical questions about building and developing MongoDB.

## LEARN MONGODB
https://university.mongodb.com/

## LICENSE
MongoDB is free and the source is available. Versions released prior to
October 16, 2018 are published under the AGPL. All versions released after
October 16, 2018, including patch fixes for prior versions, are published
under the Server Side Public License (SSPL) v1. See individual files for
details.
