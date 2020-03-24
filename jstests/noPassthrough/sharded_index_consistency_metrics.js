/*
 * Tests index consistency metrics in the serverStatus output.
 * @tags: [requires_sharding]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/sharded_index_consistency_metrics_helpers.js");

// This test creates inconsistent indexes.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

/*
 * Asserts that the serverStatus output does not contain the index consistency metrics
 * both by default and when 'shardedIndexConsistency' is explicitly included.
 */
function assertServerStatusNotContainIndexMetrics(conn) {
    let res = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    assert.eq(undefined, res.shardedIndexConsistency, tojson(res.shardedIndexConsistency));

    res = assert.commandWorked(conn.adminCommand({serverStatus: 1, shardedIndexConsistency: 1}));
    assert.eq(undefined, res.shardedIndexConsistency, tojson(res.shardedIndexConsistency));
}

/*
 * Asserts the serverStatus output for 'configPrimaryConn' has the expected number of collections
 * with inconsistent indexes and the output for 'configSecondaryConn' always reports 0. For each
 * mongod in 'connsWithoutIndexConsistencyMetrics', asserts that its serverStatus output does not
 * contain the index consistency metrics.
 */
function checkServerStatus(configPrimaryConn,
                           configSecondaryConn,
                           connsWithoutIndexConsistencyMetrics,
                           expectedNumCollsWithInconsistentIndexes) {
    // Sleep to let the periodic check run. Note this won't guarantee the check has run, but should
    // make it likely enough to catch bugs in most test runs.
    sleep(intervalMS * 2);

    checkServerStatusNumCollsWithInconsistentIndexes(configPrimaryConn,
                                                     expectedNumCollsWithInconsistentIndexes);

    // A config secondary should always report zero because only primaries run the aggregation to
    // find inconsistent indexes.
    checkServerStatusNumCollsWithInconsistentIndexes(configSecondaryConn, 0);

    for (const conn of connsWithoutIndexConsistencyMetrics) {
        assertServerStatusNotContainIndexMetrics(conn);
    }
}

const intervalMS = 500;
const st = new ShardingTest({
    shards: 2,
    config: 3,
    configOptions: {setParameter: {"shardedIndexConsistencyCheckIntervalMS": intervalMS}}
});
const dbName = "testDb";
const ns1 = dbName + ".testColl1";
const ns2 = dbName + ".testColl2";
const ns3 = dbName + ".testColl3";
const ns4 = dbName + ".testColl4";
const expiration = 1000000;
const filterExpr = {
    x: {$gt: 50}
};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns1, key: {_id: "hashed"}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns2, key: {_id: "hashed"}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns3, key: {_id: "hashed"}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns4, key: {_id: "hashed"}}));

// Disable the check on one config secondary to verify this means metrics won't be shown in
// serverStatus.
assert.commandWorked(st.config2.getDB("admin").runCommand(
    {setParameter: 1, enableShardedIndexConsistencyCheck: false}));

let configPrimaryConn = st.config0;
let configSecondaryConn = st.config1;
const connsWithoutIndexConsistencyMetrics = [st.config2, st.shard0, st.shard1, st.s];

checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 0);

// Create an inconsistent index for ns1.
assert.commandWorked(st.shard0.getCollection(ns1).createIndex({x: 1}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 1);

// Create another inconsistent index for ns1.
assert.commandWorked(st.shard1.getCollection(ns1).createIndexes([{y: 1}]));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 1);

// Create an inconsistent index for ns2.
assert.commandWorked(st.shard0.getCollection(ns2).createIndex({x: 1}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 2);

// Resolve the index inconsistency for ns2.
assert.commandWorked(st.shard1.getCollection(ns2).createIndex({x: 1}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 1);

// Create indexes with different keys for the same name and verify this is considered
// inconsistent.
assert.commandWorked(st.shard0.getCollection(ns2).createIndex({y: 1}, {name: "diffKey"}));
assert.commandWorked(st.shard1.getCollection(ns2).createIndex({z: 1}, {name: "diffKey"}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 2);

// Create indexes for n3 with the same options but in different orders on each shard, and verify
// that it is not considered as inconsistent.
assert.commandWorked(st.shard0.getCollection(ns3).createIndex({x: 1}, {
    name: "indexWithOptionsOrderedDifferently",
    partialFilterExpression: filterExpr,
    expireAfterSeconds: expiration
}));
assert.commandWorked(st.shard1.getCollection(ns3).createIndex({x: 1}, {
    name: "indexWithOptionsOrderedDifferently",
    expireAfterSeconds: expiration,
    partialFilterExpression: filterExpr
}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 2);

// Create indexes for n3 with the same key but different options on each shard, and verify that
// it is considered as inconsistent.
assert.commandWorked(st.shard0.getCollection(ns3).createIndex(
    {y: 1}, {name: "indexWithDifferentOptions", expireAfterSeconds: expiration}));
assert.commandWorked(
    st.shard1.getCollection(ns3).createIndex({y: 1}, {name: "indexWithDifferentOptions"}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 3);

// Create indexes where one is missing a property and verify this is considered inconsistent.
assert.commandWorked(st.shard0.getCollection(ns4).createIndex({y: 1}, {expireAfterSeconds: 100}));
assert.commandWorked(st.shard1.getCollection(ns4).createIndex({y: 1}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 4);

// Resolve the inconsistency on ns4.
assert.commandWorked(st.shard1.getCollection(ns4).dropIndex({y: 1}));
assert.commandWorked(st.shard1.getCollection(ns4).createIndex({y: 1}, {expireAfterSeconds: 100}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 3);

// Verify fields other than expireAfterSeconds and key are not ignored.
assert.commandWorked(st.shard0.getCollection(ns4).createIndex(
    {z: 1}, {expireAfterSeconds: 5, partialFilterExpression: {z: {$gt: 50}}}));
assert.commandWorked(st.shard1.getCollection(ns4).createIndex(
    {z: 1}, {expireAfterSeconds: 5, partialFilterExpression: {z: {$lt: 100}}}));
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 4);

//
// Verify the counter is only tracked by primaries and cleared on stepdown.
//

// Force the secondary that tracks inconsistent indexes to step up to primary and update the
// appropriate test variables.
assert.commandWorked(configSecondaryConn.adminCommand({replSetStepUp: 1}));
st.configRS.waitForState(configSecondaryConn, ReplSetTest.State.PRIMARY);
st.configRS.waitForState(configPrimaryConn, ReplSetTest.State.SECONDARY);
st.configRS.awaitNodesAgreeOnPrimary();

configSecondaryConn = configPrimaryConn;
configPrimaryConn = st.configRS.getPrimary();

// The new primary should start reporting the correct count and the old primary should start
// reporting 0.
checkServerStatus(configPrimaryConn, configSecondaryConn, connsWithoutIndexConsistencyMetrics, 4);

st.stop();

// Verify that the serverStatus output for standalones and non-sharded repilca set servers does
// not contain the index consistency metrics.
const standaloneMongod = MongoRunner.runMongod();
assertServerStatusNotContainIndexMetrics(standaloneMongod);
MongoRunner.stopMongod(standaloneMongod);

const rst = ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
assertServerStatusNotContainIndexMetrics(rst.getPrimary());
rst.stopSet();
}());
