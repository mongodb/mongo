//
// Tests that zero results are correctly returned with returnPartial and shards down
//
// Shuts down all shards, which includes the config server. Can be made to pass by restarting the
// config server, but this makes the test flaky.
// @tags: [config_shard_incompatible]
//

// Checking UUID and index consistency involves talking to shards, but this test shuts down shards.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

var checkDocCount = function(coll, returnPartialFlag, shardsDown, expectedCount) {
    assert.eq(expectedCount, coll.find({}, {}, 0, 0, 0, returnPartialFlag).itcount());
};

var st = new ShardingTest({shards: 3, mongos: 1, other: {mongosOptions: {verbose: 2}}});

// Stop balancer, we're doing our own manual chunk distribution
st.stopBalancer();

var mongos = st.s;
var admin = mongos.getDB("admin");

var collOneShard = mongos.getCollection("foo.collOneShard");
var collAllShards = mongos.getCollection("foo.collAllShards");

assert.commandWorked(admin.runCommand({enableSharding: collOneShard.getDB() + ""}));
assert.commandWorked(
    admin.runCommand({movePrimary: collOneShard.getDB() + "", to: st.shard0.shardName}));

assert.commandWorked(admin.runCommand({shardCollection: collOneShard + "", key: {_id: 1}}));
assert.commandWorked(admin.runCommand({shardCollection: collAllShards + "", key: {_id: 1}}));

// Split and move the "all shard" collection to all shards

assert.commandWorked(admin.runCommand({split: collAllShards + "", middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({split: collAllShards + "", middle: {_id: 1000}}));
assert.commandWorked(
    admin.runCommand({moveChunk: collAllShards + "", find: {_id: 0}, to: st.shard1.shardName}));
assert.commandWorked(
    admin.runCommand({moveChunk: collAllShards + "", find: {_id: 1000}, to: st.shard2.shardName}));

// Collections are now distributed correctly
jsTest.log("Collections now distributed correctly.");
st.printShardingStatus();

var inserts = [{_id: -1}, {_id: 1}, {_id: 1000}];

assert.commandWorked(collOneShard.insert(inserts));
assert.commandWorked(collAllShards.insert(inserts));

var returnPartialFlag = 1 << 7;

jsTest.log("All shards up!");

assert.eq(3, collOneShard.find().itcount());
assert.eq(3, collAllShards.find().itcount());

checkDocCount(collOneShard, returnPartialFlag, false, 3);
checkDocCount(collAllShards, returnPartialFlag, false, 3);

jsTest.log("One shard down!");

st.rs2.stopSet();

jsTest.log("done.");

checkDocCount(collOneShard, returnPartialFlag, false, 3);
checkDocCount(collAllShards, returnPartialFlag, true, 2);

jsTest.log("Two shards down!");

st.rs1.stopSet();

jsTest.log("done.");

checkDocCount(collOneShard, returnPartialFlag, false, 3);
checkDocCount(collAllShards, returnPartialFlag, true, 1);

jsTest.log("All shards down!");

st.rs0.stopSet();

jsTest.log("done.");

checkDocCount(collOneShard, returnPartialFlag, true, 0);
checkDocCount(collAllShards, returnPartialFlag, true, 0);

jsTest.log("DONE!");

st.stop();
