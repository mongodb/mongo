// This test ensures that data we add on a replica set is still accessible via mongos when we add it
// as a shard.  Then it makes sure that we can move the primary for this unsharded database to
// another shard that we add later, and after the move the data is still accessible.

(function() {
    "use strict";

    var numDocs = 10000;
    var baseName = "moveprimary-replset";
    var testDBName = baseName;
    var testCollName = 'coll';

    jsTest.log("Spinning up a sharded cluster, but not adding the shards");
    var shardingTestConfig = {
        name: baseName,
        mongos: 1,
        shards: 2,
        config: 3,
        rs: {nodes: 3},
        other: {manualAddShard: true}
    };
    var shardingTest = new ShardingTest(shardingTestConfig);

    jsTest.log("Geting connections to the individual shards");
    var replSet1 = shardingTest.rs0;
    var replSet2 = shardingTest.rs1;

    jsTest.log("Adding data to our first replica set");
    var repset1DB = replSet1.getPrimary().getDB(testDBName);
    for (var i = 1; i <= numDocs; i++) {
        repset1DB[testCollName].insert({x: i});
    }
    replSet1.awaitReplication();

    jsTest.log("Geting connection to mongos for the cluster");
    var mongosConn = shardingTest.s;
    var testDB = mongosConn.getDB(testDBName);

    jsTest.log("Adding replSet1 as only shard");
    mongosConn.adminCommand({addshard: replSet1.getURL()});

    jsTest.log(
        "Updating the data via mongos and making sure all documents are updated and present");
    testDB[testCollName].update({}, {$set: {y: 'hello'}}, false /*upsert*/, true /*multi*/);
    assert.eq(testDB[testCollName].count({y: 'hello'}),
              numDocs,
              'updating and counting docs via mongos failed');

    jsTest.log("Adding replSet2 as second shard");
    mongosConn.adminCommand({addshard: replSet2.getURL()});

    mongosConn.getDB('admin').printShardingStatus();
    printjson(replSet2.getPrimary().getDBs());

    jsTest.log("Moving test db from replSet1 to replSet2");
    assert.commandWorked(
        mongosConn.getDB('admin').runCommand({moveprimary: testDBName, to: replSet2.getURL()}));
    mongosConn.getDB('admin').printShardingStatus();
    printjson(replSet2.getPrimary().getDBs());
    assert.eq(testDB.getSiblingDB("config").databases.findOne({"_id": testDBName}).primary,
              replSet2.name,
              "Failed to change primary shard for unsharded database.");

    jsTest.log(
        "Updating the data via mongos and making sure all documents are updated and present");
    testDB[testCollName].update({}, {$set: {z: 'world'}}, false /*upsert*/, true /*multi*/);
    assert.eq(testDB[testCollName].count({z: 'world'}),
              numDocs,
              'updating and counting docs via mongos failed');

    jsTest.log("Shutting down cluster");
    shardingTest.stop();

    print('moveprimary-replset.js SUCCESS');

})();
