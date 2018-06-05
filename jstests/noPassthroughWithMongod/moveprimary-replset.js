// This test ensures that data we add on a replica set is still accessible via mongos when we add it
// as a shard.  Then it makes sure that we can move the primary for this unsharded database to
// another shard that we add later, and after the move the data is still accessible.
// @tags: [requires_replication, requires_sharding]

(function() {
    "use strict";

    var numDocs = 10000;
    var baseName = "moveprimary-replset";
    var testDBName = baseName;
    var testCollName = 'coll';

    var shardingTestConfig = {
        name: baseName,
        mongos: 1,
        shards: 2,
        config: 3,
        rs: {nodes: 3},
        other: {manualAddShard: true}
    };

    var shardingTest = new ShardingTest(shardingTestConfig);

    var replSet1 = shardingTest.rs0;
    var replSet2 = shardingTest.rs1;

    var repset1DB = replSet1.getPrimary().getDB(testDBName);
    for (var i = 1; i <= numDocs; i++) {
        repset1DB[testCollName].insert({x: i});
    }
    replSet1.awaitReplication();

    var mongosConn = shardingTest.s;
    var testDB = mongosConn.getDB(testDBName);

    mongosConn.adminCommand({addshard: replSet1.getURL()});

    testDB[testCollName].update({}, {$set: {y: 'hello'}}, false /*upsert*/, true /*multi*/);
    assert.eq(testDB[testCollName].count({y: 'hello'}),
              numDocs,
              'updating and counting docs via mongos failed');

    mongosConn.adminCommand({addshard: replSet2.getURL()});

    assert.commandWorked(
        mongosConn.getDB('admin').runCommand({moveprimary: testDBName, to: replSet2.getURL()}));
    mongosConn.getDB('admin').printShardingStatus();
    assert.eq(testDB.getSiblingDB("config").databases.findOne({"_id": testDBName}).primary,
              replSet2.name,
              "Failed to change primary shard for unsharded database.");

    testDB[testCollName].update({}, {$set: {z: 'world'}}, false /*upsert*/, true /*multi*/);
    assert.eq(testDB[testCollName].count({z: 'world'}),
              numDocs,
              'updating and counting docs via mongos failed');

    shardingTest.stop();
})();
