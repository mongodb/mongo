// Test that aggregations with the "comment" field succeed during the upgrade, in particular when
// there are mixed version shards. When a 4.4 shard is nominated as the merger during upgrade, it
// cannot propagate the comment on getMore commands. This is because any 4.2 nodes involved in the
// query are unprepared to handle the "comment" field.
//
// This is designed as a regression test for SERVER-45002.
//
// TODO SERVER-45579: Remove this test after branching for 4.5, since this is specific to the
// 4.2/4.4 upgrade/downgrade process.
//
// Checking UUID consistency uses cached connections, which are not valid across the server restarts
// done during upgrade.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster().
load("jstests/multiVersion/libs/multi_rs.js");       // For upgradeSet().

// Start with a last-stable cluster with two shards.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 3},
    other: {
        mongosOptions: {binVersion: "last-stable"},
        configOptions: {binVersion: "last-stable"},
        rsOptions: {binVersion: "last-stable"},
    }
});

let testDb = st.s.getDB("testDb");
assert.commandWorked(testDb.source.insert({_id: -1}));
assert.commandWorked(testDb.source.insert({_id: 1}));

// Shard a collection and ensure that there are chunks on both shards.
st.ensurePrimaryShard("testDb", st.shard1.shardName);
st.shardColl("source", {_id: 1}, {_id: 0}, {_id: -1}, "testDb", true);

// Runs a $merge which will use the 4.4 node as the merger, specifying the "comment" parameter.
// Ensures that the command succeeds and that the correct results are written to the output
// collection. Cleans up afterwards by dropping the $merge destination collection.
const runAggregateWithPrimaryShardMerger = function() {
    testDb = st.s.getDB("testDb");
    assert.eq(
        0,
        testDb.source
            .aggregate(
                [
                    {$_internalInhibitOptimization: {}},
                    {
                        $merge:
                            {into: "destination", whenMatched: "replace", whenNotMatched: "insert"}
                    }
                ],
                {comment: "my comment"})
            .itcount());
    assert.eq(2, testDb.destination.find().itcount());

    assert(testDb.destination.drop());
};

function upgradeSet(rs, options) {
    rs.upgradeSet(options);

    // Wait for the shard to become available.
    rs.awaitSecondaryNodes();

    // Wait for the ReplicaSetMonitor on mongoS and each shard to reflect the state of both shards.
    for (let client of [st.s, st.rs0.getPrimary(), st.rs1.getPrimary()]) {
        awaitRSClientHosts(
            client, [st.rs0.getPrimary(), st.rs1.getPrimary()], {ok: true, ismaster: true});
    }
}

function upgradeCluster(version, components) {
    st.upgradeCluster(version, components);

    // Wait for the config server and shards to become available.
    st.configRS.awaitSecondaryNodes();
    st.rs0.awaitSecondaryNodes();
    st.rs1.awaitSecondaryNodes();

    // Wait for the ReplicaSetMonitor on mongoS and each shard to reflect the state of both shards.
    for (let client of [st.s, st.rs0.getPrimary(), st.rs1.getPrimary()]) {
        awaitRSClientHosts(
            client, [st.rs0.getPrimary(), st.rs1.getPrimary()], {ok: true, ismaster: true});
    }
}

runAggregateWithPrimaryShardMerger();

// Upgrade the primary shard to "latest", and verify that the agg command still works correctly.
upgradeSet(st.rs1, {binVersion: "latest"});
runAggregateWithPrimaryShardMerger();

// Upgrade the other shard and repeat the test.
upgradeSet(st.rs0, {binVersion: "latest"});
runAggregateWithPrimaryShardMerger();

// Upgrade the config servers and repeat the test.
upgradeCluster("latest", {upgradeConfigs: true, upgradeMongos: false, upgradeShards: false});
runAggregateWithPrimaryShardMerger();

// Upgrade the mongos and repeat the test.
upgradeCluster("latest", {upgradeConfigs: false, upgradeMongos: true, upgradeShards: false});
runAggregateWithPrimaryShardMerger();

// Set the FCV to "4.4" to complete the upgrade and repeat the test.
assert.commandWorked(st.s.getDB("admin").runCommand({setFeatureCompatibilityVersion: "4.4"}));
runAggregateWithPrimaryShardMerger();

st.stop();
})();
