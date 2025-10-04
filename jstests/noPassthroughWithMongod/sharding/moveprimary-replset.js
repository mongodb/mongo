// This test ensures that data we add on a replica set is still accessible via mongos when we add it
// as a shard.  Then it makes sure that we can move the primary for this unsharded database to
// another shard that we add later, and after the move the data is still accessible.
// @tags: [
//   requires_replication,
//   requires_sharding,
// ]

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {moveDatabaseAndUnshardedColls} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

let numDocs = 10000;
let baseName = "moveprimary-replset";
let testDBName = baseName;
let testCollName = "coll";

let shardingTestConfig = {
    name: baseName,
    mongos: 1,
    shards: 2,
    config: 3,
    rs: {nodes: 3},
    other: {manualAddShard: true},
};

let shardingTest = new ShardingTest(shardingTestConfig);

let replSet1 = shardingTest.rs0;
let replSet2 = shardingTest.rs1;

let repset1DB = replSet1.getPrimary().getDB(testDBName);
for (let i = 1; i <= numDocs; i++) {
    repset1DB[testCollName].insert({x: i});
}
replSet1.awaitReplication();

let mongosConn = shardingTest.s;
let testDB = mongosConn.getDB(testDBName);

mongosConn.adminCommand({addshard: replSet1.getURL()});

testDB[testCollName].update({}, {$set: {y: "hello"}}, false /*upsert*/, true /*multi*/);
assert.eq(testDB[testCollName].count({y: "hello"}), numDocs, "updating and counting docs via mongos failed");

mongosConn.adminCommand({addshard: replSet2.getURL()});

moveDatabaseAndUnshardedColls(mongosConn.getDB(testDBName), replSet2.name);

mongosConn.getDB("admin").printShardingStatus();
assert.eq(
    testDB.getSiblingDB("config").databases.findOne({"_id": testDBName}).primary,
    replSet2.name,
    "Failed to change primary shard for unsharded database.",
);

testDB[testCollName].update({}, {$set: {z: "world"}}, false /*upsert*/, true /*multi*/);
assert.eq(testDB[testCollName].count({z: "world"}), numDocs, "updating and counting docs via mongos failed");

shardingTest.stop();
