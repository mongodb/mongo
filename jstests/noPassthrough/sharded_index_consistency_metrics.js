/*
 * Tests index consistency metrics in the serverStatus output.
 * @tags: [requires_fcv_44, requires_sharding]
 */
(function() {
"use strict";

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
 * Asserts that eventually the number of sharded collections with inconsistent indexes in the
 * serverStatus output is equal to the expected count.
 */
function checkServerStatusNumCollsWithInconsistentIndexes(conn, expectedCount) {
    assert.soon(
        () => {
            const res = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
            assert.hasFields(res, ["shardedIndexConsistency"]);
            assert.hasFields(res.shardedIndexConsistency,
                             ["numShardedCollectionsWithInconsistentIndexes"]);
            return expectedCount ==
                res.shardedIndexConsistency.numShardedCollectionsWithInconsistentIndexes;
        },
        `expect the count of sharded collections with inconsistent indexes to eventually be equal to ${
            expectedCount}`,
        undefined /* timeout */,
        1000 /* interval */);
}

/*
 * For each mongod in 'connsWithIndexConsistencyMetrics', asserts that its serverStatus
 * output has the expected number of collections with inconsistent indexes. For each mongod
 * in 'connsWithoutIndexConsistencyMetrics', asserts that its serverStatus output does
 * not contain the index consistency metrics.
 */
function checkServerStatus(connsWithIndexConsistencyMetrics,
                           connsWithoutIndexConsistencyMetrics,
                           expectedNumCollsWithInconsistentIndexes) {
    for (const conn of connsWithIndexConsistencyMetrics) {
        checkServerStatusNumCollsWithInconsistentIndexes(conn,
                                                         expectedNumCollsWithInconsistentIndexes);
    }
    for (const conn of connsWithoutIndexConsistencyMetrics) {
        assertServerStatusNotContainIndexMetrics(conn);
    }
}

const intervalMS = 3000;
const st = new ShardingTest({
    shards: 2,
    config: 2,
    configOptions: {setParameter: {"shardedIndexConsistencyCheckIntervalMS": intervalMS}}
});
const dbName = "testDb";
const ns1 = dbName + ".testColl1";
const ns2 = dbName + ".testColl2";
const ns3 = dbName + ".testColl3";
const expiration = 1000000;
const filterExpr = {
    x: {$gt: 50}
};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns1, key: {_id: "hashed"}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns2, key: {_id: "hashed"}}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns3, key: {_id: "hashed"}}));

st.config1.getDB("admin").runCommand({setParameter: 1, enableShardedIndexConsistencyCheck: false});
const connsWithIndexConsistencyMetrics = [st.config0];
const connsWithoutIndexConsistencyMetrics = [st.config1, st.shard0, st.shard1, st.s];

checkServerStatus(connsWithIndexConsistencyMetrics, connsWithoutIndexConsistencyMetrics, 0);

// Create an inconsistent index for ns1.
assert.commandWorked(st.shard0.getCollection(ns1).createIndex({x: 1}));
checkServerStatus(connsWithIndexConsistencyMetrics, connsWithoutIndexConsistencyMetrics, 1);

// Create another inconsistent index for ns1.
assert.commandWorked(st.shard1.getCollection(ns1).createIndexes([{y: 1}]));
checkServerStatus(connsWithIndexConsistencyMetrics, connsWithoutIndexConsistencyMetrics, 1);

// Create an inconsistent index for ns2.
assert.commandWorked(st.shard0.getCollection(ns2).createIndex({x: 1}));
checkServerStatus(connsWithIndexConsistencyMetrics, connsWithoutIndexConsistencyMetrics, 2);

// Resolve the index inconsistency for ns2.
assert.commandWorked(st.shard1.getCollection(ns2).createIndex({x: 1}));
checkServerStatus(connsWithIndexConsistencyMetrics, connsWithoutIndexConsistencyMetrics, 1);

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
checkServerStatus(connsWithIndexConsistencyMetrics, connsWithoutIndexConsistencyMetrics, 1);

// Create indexes for n3 with the same key but different options on each shard, and verify that
// it is considered as inconsistent.
assert.commandWorked(st.shard0.getCollection(ns3).createIndex(
    {y: 1}, {name: "indexWithDifferentOptions", expireAfterSeconds: expiration}));
assert.commandWorked(
    st.shard1.getCollection(ns3).createIndex({y: 1}, {name: "indexWithDifferentOptions"}));
checkServerStatus(connsWithIndexConsistencyMetrics, connsWithoutIndexConsistencyMetrics, 2);

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
