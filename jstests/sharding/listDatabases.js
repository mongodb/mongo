(function() {
'use strict';
var test = new ShardingTest({shards: 1, mongos: 1, other: {chunkSize: 1}});

var mongos = test.s0;
var mongod = test.shard0;

var res;
var dbArray;

// grab the config db instance by name
var getDBSection = function(dbsArray, dbToFind) {
    for (var pos in dbsArray) {
        if (dbsArray[pos].name && dbsArray[pos].name === dbToFind)
            return dbsArray[pos];
    }
    return null;
};

// Function to verify information for a database entry in listDatabases.
var dbEntryCheck = function(dbEntry, onConfig) {
    assert.neq(null, dbEntry);
    assert.neq(null, dbEntry.sizeOnDisk);
    assert.eq(false, dbEntry.empty);

    // Check against shards
    var shards = dbEntry.shards;
    assert(shards);
    assert((shards["config"] && onConfig) || (!shards["config"] && !onConfig));
};

// Non-config-server db checks.
{
    assert.commandWorked(mongos.getDB("blah").foo.insert({_id: 1}));
    assert.commandWorked(mongos.getDB("foo").foo.insert({_id: 1}));
    assert.commandWorked(mongos.getDB("raw").foo.insert({_id: 1}));

    res = mongos.adminCommand("listDatabases");
    dbArray = res.databases;

    dbEntryCheck(getDBSection(dbArray, "blah"), TestData.configShard);
    dbEntryCheck(getDBSection(dbArray, "foo"), TestData.configShard);
    dbEntryCheck(getDBSection(dbArray, "raw"), TestData.configShard);
}

// Local db is never returned.
{
    res = mongos.adminCommand("listDatabases");
    dbArray = res.databases;

    assert(!getDBSection(dbArray, 'local'));
}

// Admin and config are always reported on the config shard.
{
    assert.commandWorked(mongos.getDB("admin").test.insert({_id: 1}));
    assert.commandWorked(mongos.getDB("config").test.insert({_id: 1}));

    res = mongos.adminCommand("listDatabases");
    dbArray = res.databases;

    dbEntryCheck(getDBSection(dbArray, "config"), true);
    dbEntryCheck(getDBSection(dbArray, "admin"), true);
}

// Config db can be present on config shard and on other shards.
{
    mongod.getDB("config").foo.insert({_id: 1});

    res = mongos.adminCommand("listDatabases");
    dbArray = res.databases;

    var entry = getDBSection(dbArray, "config");
    dbEntryCheck(entry, true);
    assert(entry["shards"]);
    // There's only the "config" shard in config shard mode.
    assert.eq(Object.keys(entry["shards"]).length, TestData.configShard ? 1 : 2);
}

// Admin db is only reported on the config shard, never on other shards.
{
    mongod.getDB("admin").foo.insert({_id: 1});

    res = mongos.adminCommand("listDatabases");
    dbArray = res.databases;

    var entry = getDBSection(dbArray, "admin");
    dbEntryCheck(entry, true);
    assert(entry["shards"]);
    assert.eq(Object.keys(entry["shards"]).length, 1);
}

test.stop();
})();
