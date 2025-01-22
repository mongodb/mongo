/*
 * Basic test to validate the expected output of the $shardedDataDistribution aggregation stage.
 * @tags: [
 *   requires_2_or_more_shards,
 *   # The test cases require control over placement and tracking state of each namespace.
 *   assumes_no_track_upon_creation,
 *   assumes_unsharded_collection,
 *   assumes_balancer_off,
 *   # TODO SERVER-99707 remove the following exclusion tag.
 *   does_not_support_stepdowns,
 * ]
 */

import {
    getNumShards,
    getRandomShardName,
    setupTestDatabase
} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

// Runs $collStats and transforms its output to be easily comparable against
// $shardedDataDistribution.
function getFormattedCollStatsFor(coll) {
    const collStatsResponse = coll.aggregate([{$collStats: {storageStats: {}}}]).toArray();
    assert.neq(null, collStatsResponse);
    let formattedResponse = {};
    for (let collStatsOnShard of collStatsResponse) {
        formattedResponse[collStatsOnShard.shard] = collStatsOnShard;
    }
    return formattedResponse;
}

function crossCheckAggregationStagesFor(collections) {
    let collStatsResponse = {};
    for (let coll of collections) {
        collStatsResponse[coll.getFullName()] = getFormattedCollStatsFor(coll);
    }

    // Get data to validate
    const shardedDataDistributionResponse =
        adminDb.aggregate([{$shardedDataDistribution: {}}]).toArray();

    assert.gte(shardedDataDistributionResponse.length, collections.length);

    // Test the data obtained by $shardedDataDistribution stage
    for (const collectionDataDistribution of shardedDataDistributionResponse) {
        const ns = collectionDataDistribution.ns;

        // Check only for namespaces appearing in shardedDataDistributionResponse
        if (collStatsResponse.hasOwnProperty(ns)) {
            assert.eq(collectionDataDistribution.shards.length,
                      Object.keys(collStatsResponse[ns]).length);

            // Check for consistency of each relevant field.
            for (const shard of collectionDataDistribution.shards) {
                const shardNameFromDataDistribution = shard.shardName;
                const ownedSizeBytesFromDataDistribution = shard.ownedSizeBytes;
                const orphanedSizeBytesFromDataDistribution = shard.orphanedSizeBytes;
                const numOwnedDocumentsFromDataDistribution = shard.numOwnedDocuments;
                const numOrphanedDocsFromDataDistribution = shard.numOrphanedDocs;

                assert.eq(true,
                          collStatsResponse[ns].hasOwnProperty(shardNameFromDataDistribution));

                const avgObjSize =
                    collStatsResponse[ns][shardNameFromDataDistribution].storageStats.avgObjSize;
                const numOrphanDocs =
                    collStatsResponse[ns][shardNameFromDataDistribution].storageStats.numOrphanDocs;
                const storageStatsCount =
                    collStatsResponse[ns][shardNameFromDataDistribution].storageStats.count;

                const ownedSizeBytesFromCollStats =
                    (storageStatsCount - numOrphanDocs) * avgObjSize;
                const orphanedSizeBytesFromCollStats = numOrphanDocs * avgObjSize;
                const numOwnedDocumentsFromCollStats = storageStatsCount - numOrphanDocs;
                const numOrphanedDocsFromCollStats = numOrphanDocs;

                assert.eq(ownedSizeBytesFromDataDistribution, ownedSizeBytesFromCollStats);
                assert.eq(orphanedSizeBytesFromDataDistribution, orphanedSizeBytesFromCollStats);
                assert.eq(numOwnedDocumentsFromDataDistribution, numOwnedDocumentsFromCollStats);
                assert.eq(numOrphanedDocsFromDataDistribution, numOrphanedDocsFromCollStats);
            }
        }
    }
}

// Initialize namespaces accessed across test cases.
const adminDb = db.getSiblingDB('admin');

const primaryShard = getRandomShardName(db);
const otherShard = getRandomShardName(db, [primaryShard] /*exclude*/);

const db1 = setupTestDatabase(db, 'db1', primaryShard);
const db2 = setupTestDatabase(db, 'db2', primaryShard);
const fooColl = db1.getCollection('foo');
const bazColl = db2.getCollection('baz');
const fooNss = fooColl.getFullName();
const bazNss = bazColl.getFullName();

assert.commandWorked(db.adminCommand({enableSharding: db1.getName(), primaryShard: primaryShard}));
assert.commandWorked(db.adminCommand({shardCollection: fooNss, key: {sKey: 1}}));
assert.commandWorked(db.adminCommand({enableSharding: db2.getName(), primaryShard: primaryShard}));
assert.commandWorked(db.adminCommand({shardCollection: bazNss, key: {sKey: 1}}));

jsTest.log('The outputs of $shardedDataDistribution and $collStats are consistent');
{
    // Insert data to validate the aggregation stage
    const collectionsToTest = [fooColl, bazColl];
    const numDocs = 6;
    for (let coll of collectionsToTest) {
        for (let i = 0; i < numDocs; i++) {
            assert.commandWorked(coll.insert({sKey: i}));
        }
    }

    // Test before chunk migration
    crossCheckAggregationStagesFor(collectionsToTest);

    for (let coll of collectionsToTest) {
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {sKey: 2}}));
        assert.commandWorked(db.adminCommand({
            moveChunk: coll.getFullName(),
            find: {sKey: 2},
            to: otherShard,
            _waitForDelete: true
        }));
    }

    // Test after chunk migration
    crossCheckAggregationStagesFor(collectionsToTest);
}

jsTest.log('$shardedDataDistribution rejects invalid queries and/or values');
{
    assert.commandFailedWithCode(
        adminDb.runCommand({aggregate: 1, pipeline: [{$shardedDataDistribution: 3}], cursor: {}}),
        6789100);

    const response = assert.commandFailedWithCode(
        db1.runCommand({aggregate: 'foo', pipeline: [{$shardedDataDistribution: {}}], cursor: {}}),
        6789102);
    assert.neq(-1, response.errmsg.indexOf('$shardedDataDistribution'), response.errmsg);
    assert.neq(-1, response.errmsg.indexOf('admin database'), response.errmsg);
}

jsTest.log('Test $shardedDataDistribution followed by a $match stage on the "ns" field');
{
    assert.eq(
        1, adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: fooNss}}]).itcount());
    assert.eq(
        2,
        adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: {$in: [fooNss, bazNss]}}}])
            .itcount());
    assert.eq(
        0,
        adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: 'test.IDoNotExist'}}])
            .itcount());
}

const numShardsInCluster = getNumShards(db);

jsTest.log(
    'Test $shardedDataDistribution followed by a $match stage on the "ns" field and something else');
{
    assert.eq(1,
              adminDb
                  .aggregate([
                      {$shardedDataDistribution: {}},
                      {$match: {ns: fooNss, shards: {$size: numShardsInCluster}}}
                  ])
                  .itcount());
    assert.eq(0,
              adminDb
                  .aggregate([
                      {$shardedDataDistribution: {}},
                      {$match: {ns: fooNss, shards: {$size: numShardsInCluster + 9}}}
                  ])
                  .itcount());
}

jsTest.log(
    'Test $shardedDataDistribution followed by a $match stage on the "ns" field and other match stages');
{
    assert.eq(1,
              adminDb
                  .aggregate([
                      {$shardedDataDistribution: {}},
                      {$match: {ns: fooNss}},
                      {$match: {shards: {$size: numShardsInCluster}}}
                  ])
                  .itcount());
    assert.eq(0,
              adminDb
                  .aggregate([
                      {$shardedDataDistribution: {}},
                      {$match: {ns: fooNss}},
                      {$match: {shards: {$size: numShardsInCluster + 9}}}
                  ])
                  .itcount());
    assert.eq(1,
              adminDb
                  .aggregate([
                      {$shardedDataDistribution: {}},
                      {$match: {ns: /^db1/}},
                      {$match: {shards: {$size: numShardsInCluster}}},
                      {$match: {ns: /foo$/}},
                  ])
                  .itcount());
}

jsTest.log('Test $shardedDataDistribution followed by a $match stage unrelated to "ns"');
{
    assert.neq(
        0,
        adminDb
            .aggregate(
                [{$shardedDataDistribution: {}}, {$match: {shards: {$size: numShardsInCluster}}}])
            .itcount());
    assert.eq(0,
              adminDb
                  .aggregate([
                      {$shardedDataDistribution: {}},
                      {$match: {shards: {$size: numShardsInCluster + 9}}}
                  ])
                  .itcount());
}

jsTest.log('$shardedDataDistribution supports timeseries collections');
{
    const testDb = setupTestDatabase(db, `${jsTestName()}_timeseries`);
    const testColl = testDb.getCollection('testColl');
    const nss = testColl.getFullName();
    const bucketsNss = `${testDb.getName()}.system.buckets.${testColl.getName()}`;
    assert.commandWorked(
        db.adminCommand({shardCollection: nss, timeseries: {timeField: 'ts'}, key: {ts: 1}}));
    assert.commandWorked(testColl.insertOne({
        'metadata': {'sensorId': 5578, 'type': 'temperature'},
        'ts': ISODate('2021-05-18T00:00:00.000Z'),
        'temp': 12
    }));
    assert.eq(1,
              adminDb
                  .aggregate([
                      {$shardedDataDistribution: {}},
                      {$match: {ns: bucketsNss}},
                      {
                          $match: {
                              $and: [
                                  {'shards.numOwnedDocuments': {$eq: 1}},
                                  {'shards.ownedSizeBytes': {$eq: 435}},
                                  {'shards.orphanedSizeBytes': {$eq: 0}}
                              ]
                          }
                      }
                  ])
                  .itcount());
}

jsTest.log(
    '$shardedDataDistribution does not return info on untracked or unsplittable collections');
{
    const testDb = setupTestDatabase(db, `${jsTestName()}_unsharded`);
    const unshardedColl = testDb.getCollection('testColl');
    const nss = unshardedColl.getFullName();

    // Create an empty unsharded collection; ensure that it does not appear in the output of
    // $shardedDataDistribution.
    assert.commandWorked(testDb.runCommand({create: unshardedColl.getName()}));
    assert.eq(0,
              adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: nss}}]).itcount());

    // Move the collection to make it tracked; the namespace keeps being invisible to
    // $shardedDataDistribution.
    const nonPrimaryShard = getRandomShardName(db, [testDb.getDatabasePrimaryShardId] /*exclude*/);
    assert.commandWorked(db.adminCommand({moveCollection: nss, toShard: nonPrimaryShard}));

    assert.eq(0,
              adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: nss}}]).itcount());

    // Unspittable collections become visible once sharded.
    assert.commandWorked(db.adminCommand({shardCollection: nss, key: {_id: 1}}));
    assert.eq(1,
              adminDb.aggregate([{$shardedDataDistribution: {}}, {$match: {ns: nss}}]).itcount());
}
