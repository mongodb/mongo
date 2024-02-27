# ![Logo](docs/leaf.svg) MongoDB README

Welcome to MongoDB!

## Components

-   `mongod` - The database server.
-   `mongos` - Sharding router.
-   `mongo` - The database shell (uses interactive javascript).

## Download MongoDB

-   https://www.mongodb.com/try/download/community
-   Using homebrew `brew tap mongodb/brew`
-   Using docker image `docker pull mongo`

## Building

See [Building MongoDB](docs/building.md).

## Running

For command line options invoke:

```bash
$ ./mongod --help
```

To run a single server database:

```bash
$ sudo mkdir -p /data/db
$ ./mongod
$
$ # The mongo javascript shell connects to localhost and test database by default:
$ ./mongo
> help
```

## Installing Compass

You can install compass using the `install_compass` script packaged with MongoDB:

```bash
$ ./install_compass
```

This will download the appropriate MongoDB Compass package for your platform
and install it.

## Drivers

Client drivers for most programming languages are available at
https://docs.mongodb.com/manual/applications/drivers/. Use the shell
(`mongo`) for administrative tasks.

## Bug Reports

See https://github.com/mongodb/mongo/wiki/Submit-Bug-Reports.

## Packaging

Packages are created dynamically by the [buildscripts/packager.py](buildscripts/packager.py) script.
This will generate RPM and Debian packages.

## Learn MongoDB

-   Documentation - https://docs.mongodb.com/manual/
-   Developer Center - https://www.mongodb.com/developer/
-   MongoDB University - https://learn.mongodb.com

## Cloud Hosted MongoDB

https://www.mongodb.com/cloud/atlas

## Forums

-   https://mongodb.com/community/forums/

    Technical questions about using MongoDB.

-   https://mongodb.com/community/forums/c/server-dev

    Technical questions about building and developing MongoDB.

## LICENSE

MongoDB is free and the source is available. Versions released prior to
October 16, 2018 are published under the AGPL. All versions released after
October 16, 2018, including patch fixes for prior versions, are published
under the [Server Side Public License (SSPL) v1](LICENSE-Community.txt).
See individual files for details.
