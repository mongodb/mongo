/*
 * Test to validate the $shardedDataDistribution stage.
 *
 * @tags: [
 *   requires_fcv_62,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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

// Skip orphans check because the range deleter is disabled in this test
TestData.skipCheckOrphans = true;

// Configure initial sharding cluster
// Disable the range deleter to be able to test `numOrphanDocs`
const st = new ShardingTest({shards: 3, rs: {setParameter: {disableResumableRangeDeleter: true}}});
const mongos = st.s;

const ns1 = "test.foo";
const ns2 = "bar.baz";
const ns3 = "test.unsharded";
const ns4 = "bar.unsharded";
const ns5 = "test.coll1";
const ns6 = "bar.coll2";
const ns7 = "test.timeseriesColl";
const ns8 = "test.emptyColl";

const adminDb = mongos.getDB("admin");
const testDb = mongos.getDB("test");
const barDb = mongos.getDB("bar");
const fooColl = testDb.getCollection("foo");
const bazColl = barDb.getCollection("baz");
const unshardedColl1 = testDb.getCollection("unsharded");
const unshardedColl2 = barDb.getCollection("unsharded");
const barColl2 = barDb.getCollection("coll2");
const timeseriesColl = testDb.getCollection("timeseriesColl");
const emptyColl = testDb.getCollection("emptyColl");

st.adminCommand({enablesharding: testDb.getName(), primaryShard: st.shard1.shardName});
st.adminCommand({enablesharding: barDb.getName(), primaryShard: st.shard1.shardName});

st.adminCommand({shardcollection: ns1, key: {skey: 1}});
st.adminCommand({shardcollection: ns2, key: {skey: 1}});
st.adminCommand({shardcollection: ns5, key: {skey: 1}});
st.adminCommand({shardcollection: ns6, key: {skey: 1}});
st.adminCommand({shardcollection: ns7, timeseries: {timeField: "ts"}, key: {ts: 1}});
st.adminCommand({shardcollection: ns8, key: {skey: 1}});

// We create and move the unsharded collections to make sure they are tracked
assert.commandWorked(testDb.runCommand({create: "unsharded"}));
assert.commandWorked(
    testDb.adminCommand({moveCollection: "test.unsharded", toShard: st.shard0.shardName}));

assert.commandWorked(barDb.runCommand({create: "unsharded"}));
assert.commandWorked(
    barDb.adminCommand({moveCollection: "bar.unsharded", toShard: st.shard0.shardName}));

// Insert data to validate the aggregation stage
for (let i = 0; i < 6; i++) {
    assert.commandWorked(fooColl.insert({skey: i}));
    assert.commandWorked(bazColl.insert({skey: (i + 5)}));
    assert.commandWorked(barColl2.insert({skey: i}));
}

assert.commandWorked(timeseriesColl.insertOne({
    "metadata": {"sensorId": 5578, "type": "temperature"},
    "ts": ISODate("2021-05-18T00:00:00.000Z"),
    "temp": 12
}));

// Test before chunk migration
testShardedDataAggregationStage();

st.adminCommand({split: ns1, middle: {skey: 2}});
st.adminCommand({moveChunk: ns1, find: {skey: 2}, to: st.shard0.name, _waitForDelete: true});
st.adminCommand({split: ns2, middle: {skey: 7}});
st.adminCommand({moveChunk: ns2, find: {skey: 7}, to: st.shard0.name, _waitForDelete: true});

// Moving all the chunks outside the db primary shard for both collections.
assert.commandWorked(st.s.adminCommand({moveChunk: ns5, find: {skey: 1}, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns6, find: {skey: 1}, to: st.shard0.shardName}));
// Moving the chunk to another shard that is not the db primary to verify that shards that own
// orphans but are not the primary are also shown
assert.commandWorked(st.s.adminCommand({moveChunk: ns6, find: {skey: 1}, to: st.shard2.shardName}));

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

// Test $shardedDataDistribution followed by a $match stage on the 'ns'.
assert.eq(1, adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: ns1}}]).itcount());
assert.eq(2,
          adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: {$in: [ns1, ns2]}}}])
              .itcount());
assert.eq(0,
          adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: 'test.IDoNotExist'}}])
              .itcount());

// Test $shardedDataDistribution followed by a $match stage on the 'ns' and something else.
assert.eq(
    1,
    adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: ns1, shards: {$size: 2}}}])
        .itcount());
assert.eq(
    0,
    adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: ns1, shards: {$size: 50}}}])
        .itcount());

// Test $shardedDataDistribution followed by a $match stage on the 'ns' and other match stages.
assert.eq(
    1,
    adminDb
        .aggregate(
            [{$shardedDataDistribution: {}}, {$match: {ns: ns1}}, {$match: {shards: {$size: 2}}}])
        .itcount());
assert.eq(
    0,
    adminDb
        .aggregate(
            [{$shardedDataDistribution: {}}, {$match: {ns: ns1}}, {$match: {shards: {$size: 50}}}])
        .itcount());
assert.eq(1,
          adminDb
              .aggregate([
                  {$shardedDataDistribution: {}},
                  {$match: {ns: /^test/}},
                  {$match: {shards: {$size: 2}}},
                  {$match: {ns: /foo$/}},
              ])
              .itcount());

// Test $shardedDataDistribution followed by a $match stage unrelated to 'ns'.
assert.eq(
    0,
    adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {shards: {$size: 50}}}]).itcount());

assert.neq(
    0,
    adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {shards: {$size: 2}}}]).itcount());

// Test that verifies unsharded collections are not shown by $shardedDataDistribution
assert.eq(0,
          adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: {$in: [ns3, ns4]}}}])
              .itcount());

// Test that verifies $shardedDataDistribution returns no information about a shard that does not
// own any chunks or orphans.
assert.eq(
    1,
    adminDb
        .aggregate(
            [{$shardedDataDistribution: {}}, {$match: {ns: ns5}}, {$unwind: {path: "$shards"}}])
        .itcount());

// Test that verifies $shardedDataDistribution still returns information about the a shard that
// owned a chunk in the past and now only owns orphans.
assert.eq(
    3,
    adminDb
        .aggregate(
            [{$shardedDataDistribution: {}}, {$match: {ns: ns6}}, {$unwind: {path: "$shards"}}])
        .itcount());

// Test that verifies $shardedDataDistribution returns no information about a shard that does not
// own any chunks or orphans.
assert.eq(
    1,
    adminDb
        .aggregate(
            [{$shardedDataDistribution: {}}, {$match: {ns: ns5}}, {$unwind: {path: "$shards"}}])
        .itcount());

// Test that verifies $shardedDataDistribution still returns information about the a shard that
// owned a chunk in the past and now only owns orphans.
assert.eq(
    3,
    adminDb
        .aggregate(
            [{$shardedDataDistribution: {}}, {$match: {ns: ns6}}, {$unwind: {path: "$shards"}}])
        .itcount());

// Test that verifies that the fields returned for timeseries collections are correct.
assert.eq(1,
          adminDb
              .aggregate([
                  {$shardedDataDistribution: {}},
                  {$match: {ns: "test.system.buckets.timeseriesColl"}},
                  {
                      $match: {
                          $and: [
                              {"shards.numOwnedDocuments": {$eq: 1}},
                              {"shards.ownedSizeBytes": {$eq: 435}},
                              {"shards.orphanedSizeBytes": {$eq: 0}}
                          ]
                      }
                  }
              ])
              .itcount());

// Verify that the corresponding fields returned by $shardedDataDistribution are properly set to 0
// on an empty collection
assert.eq(1,
          adminDb
              .aggregate([
                  {$shardedDataDistribution: {}},
                  {$match: {ns: ns8}},
                  {
                      $match: {
                          $and: [
                              {"shards.numOrphanedDocs": {$eq: 0}},
                              {"shards.numOwnedDocuments": {$eq: 0}},
                              {"shards.ownedSizeBytes": {$eq: 0}},
                              {"shards.orphanedSizeBytes": {$eq: 0}}
                          ]
                      }
                  }
              ])
              .itcount());

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
    -1, response2.errmsg.indexOf("The $shardedDataDistribution stage can only be run on router"));
rsTest.stopSet();
