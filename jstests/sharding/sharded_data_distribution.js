/*
 * Test to validate the $shardedDataDistribution stage.
 *
 * @tags: [
 *   requires_fcv_62,
 * ]
 */

(function() {
'use strict';

function testShardedDataAggregationStage() {
    // Get all expected results in obj format
    const fooResults = fooColl.aggregate([{$collStats: {storageStats: {}}}]).toArray();
    assert.neq(null, fooResults);
    const bazResults = bazColl.aggregate([{$collStats: {storageStats: {}}}]).toArray();
    assert.neq(null, bazResults);

    const objFooResults = {};
    for (let fooRes of fooResults) {
        objFooResults[fooRes.shard] = fooRes;
    }

    const objBazResults = {};
    for (const bazRes of bazResults) {
        objBazResults[bazRes.shard] = bazRes;
    }

    const expectedResults = {[ns1]: objFooResults, [ns2]: objBazResults};

    // Get data to validate
    const outputData = adminDb.aggregate([{$shardedDataDistribution: {}}]).toArray();

    assert.gte(outputData.length, 2);

    // Test the data obtained by $shardedDataDistribution stage
    for (const data of outputData) {
        const ns = data.ns;

        // Check only for namespaces test.foo and bar.baz
        if (expectedResults.hasOwnProperty(ns)) {
            // Check for length
            assert.eq(data.shards.length, Object.keys(expectedResults[ns]).length);

            // Check for data
            for (const shard of data.shards) {
                const outputShardName = shard.shardName;
                const outputOwnedSizeBytes = shard.ownedSizeBytes;
                const outputOrphanedSizeBytes = shard.orphanedSizeBytes;
                const outputNumOwnedDocuments = shard.numOwnedDocuments;
                const outputNumOrphanedDocs = shard.numOrphanedDocs;

                assert.eq(true, expectedResults[ns].hasOwnProperty(outputShardName));

                const avgObjSize = expectedResults[ns][outputShardName].storageStats.avgObjSize;
                const numOrphanDocs =
                    expectedResults[ns][outputShardName].storageStats.numOrphanDocs;
                const storageStatsCount = expectedResults[ns][outputShardName].storageStats.count;

                const expectedOwnedSizeBytes = (storageStatsCount - numOrphanDocs) * avgObjSize;
                const expectedOrphanedSizeBytes = numOrphanDocs * avgObjSize;
                const expectedNumOwnedDocuments = storageStatsCount - numOrphanDocs;
                const expectedNumOrphanedDocs = numOrphanDocs;

                assert.eq(outputOwnedSizeBytes, expectedOwnedSizeBytes);
                assert.eq(outputOrphanedSizeBytes, expectedOrphanedSizeBytes);
                assert.eq(outputNumOwnedDocuments, expectedNumOwnedDocuments);
                assert.eq(outputNumOrphanedDocs, expectedNumOrphanedDocs);
            }
        }
    }
}

// Configure initial sharding cluster
const st = new ShardingTest({shards: 2});
const mongos = st.s;

const ns1 = "test.foo";
const ns2 = "bar.baz";

const adminDb = mongos.getDB("admin");
const testDb = mongos.getDB("test");
const barDb = mongos.getDB("bar");
const fooColl = testDb.getCollection("foo");
const bazColl = barDb.getCollection("baz");

st.adminCommand({enablesharding: testDb.getName(), primaryShard: st.shard1.shardName});
st.adminCommand({shardcollection: ns1, key: {skey: 1}});
st.adminCommand({enablesharding: barDb.getName(), primaryShard: st.shard1.shardName});
st.adminCommand({shardcollection: ns2, key: {skey: 1}});

// Insert data to validate the aggregation stage
for (let i = 0; i < 6; i++) {
    assert.commandWorked(fooColl.insert({skey: i}));
    assert.commandWorked(bazColl.insert({skey: (i + 5)}));
}

// Test before chunk migration
testShardedDataAggregationStage();

st.adminCommand({split: ns1, middle: {skey: 2}});
st.adminCommand({moveChunk: ns1, find: {skey: 2}, to: st.shard0.name, _waitForDelete: true});
st.adminCommand({split: ns2, middle: {skey: 7}});
st.adminCommand({moveChunk: ns2, find: {skey: 7}, to: st.shard0.name, _waitForDelete: true});

// Test after chunk migration
testShardedDataAggregationStage();

// Test invalid queries/values.
assert.commandFailedWithCode(
    adminDb.runCommand({aggregate: 1, pipeline: [{$shardedDataDistribution: 3}], cursor: {}}),
    6789100);

const response = assert.commandFailedWithCode(
    testDb.runCommand({aggregate: "foo", pipeline: [{$shardedDataDistribution: {}}], cursor: {}}),
    6789102);
assert.neq(-1, response.errmsg.indexOf("$shardedDataDistribution"), response.errmsg);
assert.neq(-1, response.errmsg.indexOf("admin database"), response.errmsg);

st.stop();

// Test that verifies the behavior in unsharded deployments
const rsTest = new ReplSetTest({name: 'replicaSetTest', nodes: 2});
rsTest.startSet();
rsTest.initiate();

const primary = rsTest.getPrimary();
const admin = primary.getDB('admin');

const response2 = assert.commandFailedWithCode(
    admin.runCommand({aggregate: 1, pipeline: [{$shardedDataDistribution: {}}], cursor: {}}),
    6789101);
assert.neq(
    -1, response2.errmsg.indexOf("The $shardedDataDistribution stage can only be run on mongoS"));

rsTest.stopSet();
})();
