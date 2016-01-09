# MongoDB

Welcome to MongoDB!

## Components

- mongod - The database process.
- mongos - Sharding controller.
- mongo  - The database shell (uses interactive javascript).

## Utilities

- mongodump - MongoDB dump tool - for backups, snapshots, etc.
- mongorestore - MongoDB restore a dump
- mongoexport - Export a single collection to test (JSON, CSV)
- mongoimport - Import from JSON or CSV
- mongofiles - Utility for putting and getting files from MongoDB GridFS
- mongostat - Show performance statistics

## Building

See docs/building.md, also www.mongodb.org search for "Building".

## Running

For command line options invoke:

```bash
$ ./mongod --help
```

To run a single server database:

```bash
$ mkdir /data/db
$ ./mongod
$
$ # The mongo javascript shell connects to localhost and test database by default:
$ ./mongo 
> help
```

## Drivers

Client drivers for most programming languages are available at mongodb.org.  Use the shell ("mongo") for administrative tasks.

## Packaging

Packages are created dynamically by the package.py script located in the buildscripts directory. This will generate RPM and Debian packages.

## Important Links

- [Documentation](http://www.mongodb.org/)
- [Cloud managed MongoDB](http://cloud.mongodb.com/)
- [Mail list and IRC](http://dochub.mongodb.org/core/community)
- [Learn MongoDB](http://university.mongodb.com/)

## 32 Bit Build Notes

MongoDB uses memory mapped files.  If built as a 32 bit executable, you will not be able to work with large (multi-gigabyte) databases.  However, 32 bit builds work fine with small development databases.

## License

Most MongoDB source files (src/mongo folder and below) are made available under the terms of the GNU Affero General Public License (AGPL).  See individual files for details.

As an exception, the files in the client/, debian/, rpm/, utils/mongoutils, and all subdirectories thereof are made available under the terms of the Apache License, version 2.0.
