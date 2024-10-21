// Tests operation of the cluster when the config servers have no primary and thus the cluster
// metadata is in read-only mode.

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Checking UUID and index consistency involves talking to the config server primary, but there is
// no config server primary by the end of this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

// The routing table consistency check runs with 'snapshot' level readConcern. This readConcern
// level cannot be satisfied without a replica set primary, which we won't have on the config server
// because this test removes the config server primary.
TestData.skipCheckRoutingTableConsistency = true;

var st = new ShardingTest({
    shards: 1,
    config: 3,
    other: {
        c0: {},  // Make sure 1st config server is primary
        c1: {rsConfig: {priority: 0}},
        c2: {rsConfig: {priority: 0}}
    },
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

assert.eq(st.config0, st.configRS.getPrimary());

// Create the "test" database while the cluster metadata is still writeable.
st.s.getDB('test').foo.insert({a: 1});

// Take down two of the config servers so the remaining one goes into SECONDARY state.
st.configRS.stop(1);
st.configRS.stop(2);
st.configRS.awaitNoPrimary();

jsTestLog("Starting a new mongos when the config servers have no primary which should work");
var mongos2 = MongoRunner.runMongos({configdb: st.configRS.getURL()});
assert.neq(null, mongos2);

var testOps = function(mongos) {
    jsTestLog("Doing ops that don't require metadata writes and thus should succeed against: " +
              mongos);
    if (TestData.configShard) {
        // In config shard mode there's also only one shard node up with no primary, so just verify
        // we can still do ops on a secondary that don't require metadata.
        mongos.setSecondaryOk(true);
        assert.eq(1, mongos.getDB('test').foo.count());
        mongos.setSecondaryOk(false);
    } else {
        var initialCount = mongos.getDB('test').foo.count();
        // In config shard mode there's no primary.
        assert.commandWorked(mongos.getDB('test').foo.insert({a: 1}));
        assert.eq(initialCount + 1, mongos.getDB('test').foo.count());
    }

    assert.throws(function() {
        mongos.getDB('config').shards.findOne();
    });
    mongos.setSecondaryOk();
    var shardDoc = mongos.getDB('config').shards.findOne();
    mongos.setSecondaryOk(false);
    assert.neq(null, shardDoc);

    jsTestLog("Doing ops that require metadata writes and thus should fail against: " + mongos);
    assert.writeError(mongos.getDB("newDB").foo.insert({a: 1}));
};

testOps(mongos2);
testOps(st.s);

st.stop();
MongoRunner.stopMongos(mongos2);
