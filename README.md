# MongoDB

**Welcome to MongoDB!**

### Components

- **mongod** - The database server.
- **mongos** - Sharding router.
- **mongo**  - The database shell (uses interactive javascript).

### Utilities

- `mongodump` - Create a binary dump of the contents of a database.
- `mongorestore` - Restore data from the output created by mongodump.
- `mongoexport` - Export the contents of a collection to JSON or CSV.
- `mongoimport` - Import data from JSON, CSV or TSV.
- `mongofiles` - Put, get and delete files from GridFS.
- `mongostat` - Show the status of a running mongod/mongos.
- `bsondump` - Convert BSON files into human-readable formats.
- `mongoreplay` - Traffic capture and replay tool.
- `mongotop` - Track time spent reading and writing data.
- `install_compass` - Installs MongoDB Compass for your platform.

### Building

  See [docs/building.md](./docs/building.md).

### Running

For command line options invoke:

```sh
$ ./mongod --help
```

To run a single server database:

```sh
$ sudo mkdir -p /data/db
$ ./mongod
$
$ # The mongo javascript shell connects to localhost and test database by default:
$ ./mongo
> help
```

### Installing Compass

You can install compass using the install_compass script packaged with MongoDB:

```sh
$ ./install_compass
```

This will download the appropriate MongoDB Compass package for your platform 
and install it.

### Drivers

Client drivers for most programming languages are available at
[docs.mongodb.com/manual/applications/drivers](https://docs.mongodb.com/manual/applications/drivers/). Use the shell
(`mongo`) for administrative tasks.

### Bug Reports

See [github.com/mongodb/mongo/wiki/Submit-Bug-Reports](https://github.com/mongodb/mongo/wiki/Submit-Bug-Reports).

### Packaging

Packages are created dynamically by the `package.py` script located in the
`buildscripts/` directory. This will generate RPM and Debian packages.

### Documentation

[docs.mongodb.com/manual](https://docs.mongodb.com/manual/)

### Cloud Hosted MongoDB

[mongodb.com/cloud/atlas](https://www.mongodb.com/cloud/atlas)

### Mail Lists

[Google Groups: mongodb-user](https://groups.google.com/forum/#!forum/mongodb-user)
A forum for technical questions about using MongoDB.

[Google Groups: mongodb-dev](https://groups.google.com/forum/#!forum/mongodb-dev)
A forum for technical questions about building and developing MongoDB.

### Learn MongoDB

[MongoDB University](https://university.mongodb.com/)

## LICENSE

  MongoDB is free and the source is available. Versions released prior to
  October 16, 2018 are published under the AGPL. All versions released after
  October 16, 2018, including patch fixes for prior versions, are published
  under the Server Side Public License (SSPL) v1. See individual files for
  details.
