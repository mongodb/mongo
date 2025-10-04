/**
 * @tags: [
 * # This test adds the first shard after populating it, which is prohibited for config shards
 * config_shard_incompatible,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The UUID and index check must be able to contact the shard primaries, but this test manually
// stops 2/3 nodes of a replica set.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

// The routing table consistency check runs with 'snapshot' level readConcern. This readConcern
// level cannot be satisfied without a replica set primary, which we won't have because this test
// removes the replica set primary from a shard.
TestData.skipCheckRoutingTableConsistency = true;

let numDocs = 2000;
let baseName = "shard_insert_getlasterror_w2";
let testDBName = baseName;
let testCollName = "coll";
let replNodes = 3;

// ~1KB string
let textString = "";
for (let i = 0; i < 40; i++) {
    textString += "abcdefghijklmnopqrstuvwxyz";
}

// Spin up a sharded cluster, but do not add the shards
let shardingTestConfig = {
    name: baseName,
    mongos: 1,
    shards: 1,
    rs: {nodes: replNodes},
    other: {manualAddShard: true},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
};
let shardingTest = new ShardingTest(shardingTestConfig);

// Get connection to the individual shard
let replSet1 = shardingTest.rs0;

// Add data to it
let testDBReplSet1 = replSet1.getPrimary().getDB(testDBName);
let bulk = testDBReplSet1.foo.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({x: i, text: textString});
}
assert.commandWorked(bulk.execute());

// Get connection to mongos for the cluster
let mongosConn = shardingTest.s;
let testDB = mongosConn.getDB(testDBName);

// Add replSet1 as only shard
assert.commandWorked(mongosConn.adminCommand({addshard: replSet1.getURL()}));

// Enable sharding on test db and its collection foo
assert.commandWorked(mongosConn.getDB("admin").runCommand({enablesharding: testDBName}));
testDB[testCollName].createIndex({x: 1});
assert.commandWorked(
    mongosConn.getDB("admin").runCommand({shardcollection: testDBName + "." + testCollName, key: {x: 1}}),
);

// Test case where GLE should return an error
assert.commandWorked(testDB.foo.insert({_id: "a", x: 1}));
assert.writeError(testDB.foo.insert({_id: "a", x: 1}, {writeConcern: {w: 2, wtimeout: 30000}}));

// Add more data
bulk = testDB.foo.initializeUnorderedBulkOp();
for (let i = numDocs; i < 2 * numDocs; i++) {
    bulk.insert({x: i, text: textString});
}
assert.commandWorked(bulk.execute({w: replNodes, wtimeout: 30000}));

// Take down two nodes and make sure secondaryOk reads still work
let primary = replSet1.getPrimary();
let [secondary1, secondary2] = replSet1.getSecondaries();
replSet1.stop(secondary1);
replSet1.stop(secondary2);
replSet1.awaitSecondaryNodes(null, [primary]);

testDB.getMongo().adminCommand({setParameter: 1, logLevel: 1});
testDB.getMongo().setSecondaryOk();
print("trying some queries");
assert.soon(
    function () {
        try {
            testDB.foo.find().next();
        } catch (e) {
            print(e);
            return false;
        }
        return true;
    },
    "Queries took too long to complete correctly.",
    2 * 60 * 1000,
);

// Shutdown cluster
shardingTest.stop();

print("shard_insert_getlasterror_w2.js SUCCESS");
