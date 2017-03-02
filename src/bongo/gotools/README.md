BongoDB Tools
===================================

 - **bsondump** - _display BSON files in a human-readable format_
 - **bongoimport** - _Convert data from JSON, TSV or CSV and insert them into a collection_
 - **bongoexport** - _Write an existing collection to CSV or JSON format_
 - **bongodump/bongorestore** - _Dump BongoDB backups to disk in .BSON format, or restore them to a live database_
 - **bongostat** - _Monitor live BongoDB servers, replica sets, or sharded clusters_
 - **bongofiles** - _Read, write, delete, or update files in [GridFS](http://docs.bongodb.org/manual/core/gridfs/)_
 - **bongooplog** - _Replay oplog entries between BongoDB servers_
 - **bongotop** - _Monitor read/write activity on a bongo server_

Report any bugs, improvements, or new feature requests at https://jira.bongodb.org/browse/TOOLS

Setup
---------------
Clone the repo and run `. ./set_gopath.sh` (`set_gopath.bat` on Windows) to setup your GOPATH:

```
git clone https://github.com/bongodb/bongo-tools
cd bongo-tools
. ./set_gopath.sh
```

Building Tools
---------------
To build the tools, you need to have Go version 1.3 and up.

An additional flag, `-tags`, can be passed to the `go build` command in order to build the tools with support for SSL and/or SASL. For example:

```
mkdir bin
go build -o bin/bongoimport bongoimport/main/bongoimport.go # build bongoimport
go build -o bin/bongoimport -tags ssl bongoimport/main/bongoimport.go # build bongoimport with SSL support enabled
go build -o bin/bongoimport -tags "ssl sasl" bongoimport/main/bongoimport.go # build bongoimport with SSL and SASL support enabled
```

Contributing
---------------
See our [Contributor's Guide](CONTRIBUTING.md).

Documentation
---------------
See the BongoDB packages [documentation](http://docs.bongodb.org/master/reference/program/).

