// tests that listDatabases doesn't show config db on a shard, even if it is there

var test = new ShardingTest({shards: 1, bongos: 1, other: {chunkSize: 1}});

var bongos = test.s0;
var bongod = test.shard0;

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

assert.writeOK(bongos.getDB("blah").foo.insert({_id: 1}));
assert.writeOK(bongos.getDB("foo").foo.insert({_id: 1}));
assert.writeOK(bongos.getDB("raw").foo.insert({_id: 1}));

// verify that the config db is not on a shard
var res = bongos.adminCommand("listDatabases");
var dbArray = res.databases;
dbInConfigEntryCheck(getDBSection(dbArray, "config"));

// Local database should never be returned
var localSection = getDBSection(dbArray, 'local');
assert(!localSection);

// add doc in admin db on the config server.
assert.writeOK(bongos.getDB('admin').test.insert({_id: 1}));
res = bongos.adminCommand("listDatabases");
dbArray = res.databases;
dbInConfigEntryCheck(getDBSection(dbArray, "config"));
dbInConfigEntryCheck(getDBSection(dbArray, 'admin'));

// add doc in config/admin db on the shard
bongod.getDB("config").foo.insert({_id: 1});
bongod.getDB("admin").foo.insert({_id: 1});

// add doc in admin db (via bongos)
bongos.getDB("admin").foo.insert({_id: 1});

// verify that the config db is not on a shard
res = bongos.adminCommand("listDatabases");
dbArray = res.databases;
// check config db
assert(getDBSection(dbArray, "config"), "config db not found! 2");
assert(!getDBSection(dbArray, "config").shards, "config db is on a shard! 2");
// check admin db
assert(getDBSection(dbArray, "admin"), "admin db not found! 2");
assert(!getDBSection(dbArray, "admin").shards, "admin db is on a shard! 2");

test.stop();
