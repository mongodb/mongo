##MongoDB README

####Welcome to MongoDB!

#####COMPONENTS

&nbsp;&nbsp;&nbsp;&nbsp;````mongod```` - The database process. <br />
&nbsp;&nbsp;&nbsp;&nbsp;````mongos```` - Sharding controller. <br />
&nbsp;&nbsp;&nbsp;&nbsp;````mongo````  - The database shell (uses interactive javascript). <br />

#####UTILITIES

&nbsp;&nbsp;&nbsp;&nbsp;````mongodump````         - MongoDB dump tool - for backups, snapshots, etc. <br />
&nbsp;&nbsp;&nbsp;&nbsp;````mongorestore````      - MongoDB restore a dump <br />
&nbsp;&nbsp;&nbsp;&nbsp;````mongoexport````       - Export a single collection to test (JSON, CSV) <br />
&nbsp;&nbsp;&nbsp;&nbsp;````mongoimport````       - Import from JSON or CSV <br />
&nbsp;&nbsp;&nbsp;&nbsp;````mongofiles````        - Utility for putting and getting files from MongoDB GridFS <br />
&nbsp;&nbsp;&nbsp;&nbsp;````mongostat````         - Show performance statistics <br />

#####BUILDING

&nbsp;&nbsp;See docs/building.md, also www.mongodb.org search for "Building".

#####RUNNING

&nbsp;&nbsp;For command line options invoke:

````bash
$ ./mongod --help
````

&nbsp;&nbsp;To run a single server database:
````bash
$ mkdir /data/db
$ ./mongod
$
$ # The mongo javascript shell connects to localhost and test database by default:
$ ./mongo 
> help
````

#####DRIVERS

&nbsp;&nbsp;&nbsp;&nbsp;Client drivers for most programming languages are available at mongodb.org.  Use the shell ("mongo") for administrative tasks.

#####PACKAGING

  Packages are created dynamically by the package.py script located in the buildscripts directory. This will 
  generate RPM and Debian packages.

#####DOCUMENTATION

  http://www.mongodb.org/
 
#####CLOUD MANAGED MONGODB

  http://mms.mongodb.com/

#####MAIL LISTS AND IRC

  http://dochub.mongodb.org/core/community
  
#####LEARN MONGODB

  http://university.mongodb.com/

#####32 BIT BUILD NOTES

  MongoDB uses memory mapped files.  If built as a 32 bit executable, you will
  not be able to work with large (multi-gigabyte) databases.  However, 32 bit
  builds work fine with small development databases.

#####LICENSE

  Most MongoDB source files (src/mongo folder and below) are made available under the terms of the
  GNU Affero General Public License (AGPL).  See individual files for
  details.

  As an exception, the files in the client/, debian/, rpm/,
  utils/mongoutils, and all subdirectories thereof are made available under
  the terms of the Apache License, version 2.0.
