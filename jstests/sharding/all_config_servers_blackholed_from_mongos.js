/**
 * Ensures that if the config servers are blackholed from the point of view of MongoS, metadata
 * operations do not get stuck forever.
 *
 * Checking UUID and index consistency involves talking to config servers through mongos, but mongos
 * is blackholed from the config servers in this test.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

let st = new ShardingTest({
    shards: 2,
    mongos: 1,
    useBridge: true,
    bridgeOptions: {verbose: "vvv"},
    // ShardingTest use a high config command timeout to avoid spurious failures but this test
    // intentionally triggers a timeout, so we restore the default value.
    other: {mongosOptions: {setParameter: {defaultConfigCommandTimeoutMS: 30000}}},
});

let testDB = st.s.getDB("BlackHoleDB");

assert.commandWorked(testDB.adminCommand({enableSharding: "BlackHoleDB"}));
assert.commandWorked(testDB.adminCommand({shardCollection: testDB.ShardedColl.getFullName(), key: {_id: 1}}));

assert.commandWorked(testDB.ShardedColl.insert({a: 1}));

jsTest.log("Making all the config servers appear as a blackhole to mongos");
st.forEachConfigServer((configSvr) => {
    configSvr.discardMessagesFrom(st.s, 1.0);
});

assert.commandWorked(testDB.adminCommand({flushRouterConfig: 1}));

// This shouldn't stall
jsTest.log("Doing read operation on the sharded collection");
assert.throws(function () {
    testDB.ShardedColl.find({}).maxTimeMS(15000).itcount();
});

// This should fail, because the primary is not available
jsTest.log("Doing write operation on a new database and collection");
assert.writeError(
    st.s
        .getDB("NonExistentDB")
        .TestColl.insert({_id: 0, value: "This value will never be inserted"}, {maxTimeMS: 15000}),
);

st.stop();
