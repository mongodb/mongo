# MongoDB Tools

This is the 2.7.x **unstable** branch. This project is a work in progress.
 
 - **bsondump** - _display BSON files in a human-readable format_
 - **mongoimport** - _Convert data from JSON or CSV and insert them into a collection_
 - **mongoexport** - _Write an existing collection to CSV or JSON format_
 - **mongodump/mongorestore** - _Dump MongoDB backups to disk in .BSON format, or restore them to a live database_
 - **mongostat** - _Monitor live MongoDB servers, replica sets, or sharded clusters_
 - **mongofiles** - _Read, write, delete, or update files in [GridFS](http://docs.mongodb.org/manual/core/gridfs/)_
 - **mongooplog** - _Replay oplog entries between MongoDB servers_
 - **mongotop** - _Monitor read/write activity on a mongo server_

Report any bugs, improvements, or new feature requests at https://jira.mongodb.org/browse/tools

 
####Setup

Clone the repo and set your GOPATH to the directory root. Then run the dependency installation script.

```
git clone https://github.com/mongodb/mongo-tools
cd mongo-tools
export GOPATH=`pwd`
./gpm install
```

_Note: 'go get' is not currently compatible with this repo. This may or may not change._

#### Building Tools

```
for i in bsondump mongostat mongofiles mongoexport mongoimport mongorestore mongodump mongotop mongooplog; do
    go build  -o "$i" src/github.com/mongodb/mongo-tools/$i/main/$i.go
done
```

#### SSL

These tools also support linking with OpenSSL to provide SSL support. To build binaries with SSL features, add the `-tags ssl` argument to go build commands.

