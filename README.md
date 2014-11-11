# MongoDB Tools

This is the 2.7.x **unstable** branch. This project is a work in progress.
 
 - **bsondump** - _display BSON files in a human-readable format_
 - **mongoimport** - _Convert data from JSON, TSV or CSV and insert them into a collection_
 - **mongoexport** - _Write an existing collection to CSV or JSON format_
 - **mongodump/mongorestore** - _Dump MongoDB backups to disk in .BSON format, or restore them to a live database_
 - **mongostat** - _Monitor live MongoDB servers, replica sets, or sharded clusters_
 - **mongofiles** - _Read, write, delete, or update files in [GridFS](http://docs.mongodb.org/manual/core/gridfs/)_
 - **mongooplog** - _Replay oplog entries between MongoDB servers_
 - **mongotop** - _Monitor read/write activity on a mongo server_

Report any bugs, improvements, or new feature requests at https://jira.mongodb.org/browse/tools

 
####Setup

Clone the repo and set your GOPATH to include the vendored dependencies by using the helper script `set_gopath.sh`

```
git clone https://github.com/mongodb/mongo-tools
cd mongo-tools
. set_gopath.sh
```

#### Building Tools

To build the tools, you need to have Go version 1.3 and up. Running the `build.sh` script will install all the tools to `./bin`.

Alternatively, you can set GOBIN and use `go install`:

```
export GOBIN=bin
go install mongodump/main/mongodump.go
```

An additional flag, `-tags`, can be passed to the `go install` command in order to install the tools with support for SSL and/or SASL. For example:

```
go install -tags ssl mongoimport/main/mongoimport.go # install mongoimport with SSL support enabled
go install -tags sasl mongoimport/main/mongoimport.go # install mongoimport with SASL support enabled
go install -tags "ssl sasl" mongoimport/main/mongoimport.go # install mongoimport with both SASL and SSL
```

Alternatively, you can run `build.sh` with the appropriate arguments. For example:

```
build.sh ssl # install all the tools with SSL support enabled
build.sh sasl ssl # install all the tools with both SASL and SSL support enabled
```