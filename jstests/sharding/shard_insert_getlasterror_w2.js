// replica set as solo shard
// TODO: Add assertion code that catches hang

// The UUID and index check must be able to contact the shard primaries, but this test manually
// stops 2/3 nodes of a replica set.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

(function() {
"use strict";

var numDocs = 2000;
var baseName = "shard_insert_getlasterror_w2";
var testDBName = baseName;
var testCollName = 'coll';
var replNodes = 3;

// ~1KB string
var textString = '';
for (var i = 0; i < 40; i++) {
    textString += 'abcdefghijklmnopqrstuvwxyz';
}

// Spin up a sharded cluster, but do not add the shards
var shardingTestConfig =
    {name: baseName, mongos: 1, shards: 1, rs: {nodes: replNodes}, other: {manualAddShard: true}};
var shardingTest = new ShardingTest(shardingTestConfig);

// Get connection to the individual shard
var replSet1 = shardingTest.rs0;

// Add data to it
var testDBReplSet1 = replSet1.getPrimary().getDB(testDBName);
var bulk = testDBReplSet1.foo.initializeUnorderedBulkOp();
for (var i = 0; i < numDocs; i++) {
    bulk.insert({x: i, text: textString});
}
assert.commandWorked(bulk.execute());

// Get connection to mongos for the cluster
var mongosConn = shardingTest.s;
var testDB = mongosConn.getDB(testDBName);

// Add replSet1 as only shard
if (!TestData.configShard) {
    assert.commandWorked(mongosConn.adminCommand({addshard: replSet1.getURL()}));
} else {
    assert.commandWorked(mongosConn.adminCommand({transitionFromDedicatedConfigServer: 1}));
}

// Enable sharding on test db and its collection foo
assert.commandWorked(mongosConn.getDB('admin').runCommand({enablesharding: testDBName}));
testDB[testCollName].createIndex({x: 1});
assert.commandWorked(mongosConn.getDB('admin').runCommand(
    {shardcollection: testDBName + '.' + testCollName, key: {x: 1}}));

// Test case where GLE should return an error
assert.commandWorked(testDB.foo.insert({_id: 'a', x: 1}));
assert.writeError(testDB.foo.insert({_id: 'a', x: 1}, {writeConcern: {w: 2, wtimeout: 30000}}));

// Add more data
bulk = testDB.foo.initializeUnorderedBulkOp();
for (var i = numDocs; i < 2 * numDocs; i++) {
    bulk.insert({x: i, text: textString});
}
assert.commandWorked(bulk.execute({w: replNodes, wtimeout: 30000}));

// Take down two nodes and make sure secondaryOk reads still work
var primary = replSet1.getPrimary();
var [secondary1, secondary2] = replSet1.getSecondaries();
replSet1.stop(secondary1);
replSet1.stop(secondary2);
replSet1.waitForState(primary, ReplSetTest.State.SECONDARY);

testDB.getMongo().adminCommand({setParameter: 1, logLevel: 1});
testDB.getMongo().setSecondaryOk();
print("trying some queries");
assert.soon(function() {
    try {
        testDB.foo.find().next();
    } catch (e) {
        print(e);
        return false;
    }
    return true;
}, "Queries took too long to complete correctly.", 2 * 60 * 1000);

// Shutdown cluster
shardingTest.stop();

print('shard_insert_getlasterror_w2.js SUCCESS');
})();
