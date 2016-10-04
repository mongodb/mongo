// tests that listDatabases doesn't show config db on a shard, even if it is there

var test = new ShardingTest({shards: 1, mongos: 1, other: {chunkSize: 1}});

var mongos = test.s0;
var mongod = test.shard0;

// grab the config db instance by name
var getDBSection = function(dbsArray, dbToFind) {
    for (var pos in dbsArray) {
        if (dbsArray[pos].name && dbsArray[pos].name === dbToFind)
            return dbsArray[pos];
    }
    return null;
};

var dbInConfigEntryCheck = function(dbEntry) {
    assert.neq(null, dbEntry);
    assert(!dbEntry.shards);  // db should not be in shard.
    assert.neq(null, dbEntry.sizeOnDisk);
    assert.eq(false, dbEntry.empty);
};

assert.writeOK(mongos.getDB("blah").foo.insert({_id: 1}));
assert.writeOK(mongos.getDB("foo").foo.insert({_id: 1}));
assert.writeOK(mongos.getDB("raw").foo.insert({_id: 1}));

// verify that the config db is not on a shard
var res = mongos.adminCommand("listDatabases");
var dbArray = res.databases;
dbInConfigEntryCheck(getDBSection(dbArray, "config"));

// Local database should never be returned
var localSection = getDBSection(dbArray, 'local');
assert(!localSection);

// add doc in admin db on the config server.
assert.writeOK(mongos.getDB('admin').test.insert({_id: 1}));
res = mongos.adminCommand("listDatabases");
dbArray = res.databases;
dbInConfigEntryCheck(getDBSection(dbArray, "config"));
dbInConfigEntryCheck(getDBSection(dbArray, 'admin'));

// add doc in config/admin db on the shard
mongod.getDB("config").foo.insert({_id: 1});
mongod.getDB("admin").foo.insert({_id: 1});

// add doc in admin db (via mongos)
mongos.getDB("admin").foo.insert({_id: 1});

// verify that the config db is not on a shard
res = mongos.adminCommand("listDatabases");
dbArray = res.databases;
// check config db
assert(getDBSection(dbArray, "config"), "config db not found! 2");
assert(!getDBSection(dbArray, "config").shards, "config db is on a shard! 2");
// check admin db
assert(getDBSection(dbArray, "admin"), "admin db not found! 2");
assert(!getDBSection(dbArray, "admin").shards, "admin db is on a shard! 2");

test.stop();
