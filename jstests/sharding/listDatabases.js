// tests that listDatabases doesn't show config db on a shard, even if it is there

var test = new ShardingTest({shards: 1, mongos: 1, config: 1, other: {chunksize:1, separateConfig:true}})

var mongos = test.s0
var mongod = test.shard0;

//grab the config db instance by name
var getDBSection = function (dbsArray, dbToFind) {
    for(var pos in dbsArray) {
        if (dbsArray[pos].name && dbsArray[pos].name === dbToFind)
            return dbsArray[pos];
    }
    return null;
}

mongos.getDB("blah").foo.insert({_id:1})
mongos.getDB("foo").foo.insert({_id:1})
mongos.getDB("raw").foo.insert({_id:1})
//wait for writes to finish
mongos.getDB("raw").getLastError()

//verify that the config db is not on a shard
var res = mongos.adminCommand("listDatabases");
var dbArray = res.databases;
assert(getDBSection(dbArray, "config"), "config db not found! 1")
assert(!getDBSection(dbArray, "config").shards, "config db is on a shard! 1")

//add doc in config/admin db on the shard
mongod.getDB("config").foo.insert({_id:1})
mongod.getDB("admin").foo.insert({_id:1})

//add doc in admin db (via mongos)
mongos.getDB("admin").foo.insert({_id:1})

//verify that the config db is not on a shard
var res = mongos.adminCommand("listDatabases");
var dbArray = res.databases;
//check config db
assert(getDBSection(dbArray, "config"), "config db not found! 2")
assert(!getDBSection(dbArray, "config").shards, "config db is on a shard! 2")
//check admin db
assert(getDBSection(dbArray, "admin"), "admin db not found! 2")
assert(!getDBSection(dbArray, "admin").shards, "admin db is on a shard! 2")

test.stop()