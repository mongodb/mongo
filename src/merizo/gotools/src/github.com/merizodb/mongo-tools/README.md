MerizoDB Tools
===================================

 - **bsondump** - _display BSON files in a human-readable format_
 - **merizoimport** - _Convert data from JSON, TSV or CSV and insert them into a collection_
 - **merizoexport** - _Write an existing collection to CSV or JSON format_
 - **merizodump/merizorestore** - _Dump MerizoDB backups to disk in .BSON format, or restore them to a live database_
 - **merizostat** - _Monitor live MerizoDB servers, replica sets, or sharded clusters_
 - **merizofiles** - _Read, write, delete, or update files in [GridFS](http://docs.merizodb.org/manual/core/gridfs/)_
 - **merizotop** - _Monitor read/write activity on a merizo server_
 - **merizoreplay** - _Capture, observe, and replay traffic for MerizoDB_


Report any bugs, improvements, or new feature requests at https://jira.merizodb.org/browse/TOOLS

Setup
---------------
Clone the repo and run `. ./set_gopath.sh` (`set_gopath.bat` on Windows) to setup your GOPATH:

```
git clone https://github.com/merizodb/merizo-tools
cd merizo-tools
. ./set_gopath.sh
```

Building Tools
---------------
To build the tools, you need to have Go version 1.3 and up.

An additional flag, `-tags`, can be passed to the `go build` command in order to build the tools with support for SSL and/or SASL. For example:

```
mkdir bin
go build -o bin/merizoimport merizoimport/main/merizoimport.go # build merizoimport
go build -o bin/merizoimport -tags ssl merizoimport/main/merizoimport.go # build merizoimport with SSL support enabled
go build -o bin/merizoimport -tags "ssl sasl" merizoimport/main/merizoimport.go # build merizoimport with SSL and SASL support enabled
```

Contributing
---------------
See our [Contributor's Guide](CONTRIBUTING.md).

Documentation
---------------
See the MerizoDB packages [documentation](http://docs.merizodb.org/master/reference/program/).

