/**
 * Test inserts into sharded timeseries collection.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_find_command,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'hostid';

// Connections.
const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;

// Sanity checks.
if (!TimeseriesTest.timeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

// Databases and collections.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
const mainDB = mongos.getDB(dbName);

// Helpers.
let currentId = 0;
function generateId() {
    return currentId++;
}

function generateBatch(size) {
    return TimeseriesTest.generateHosts(size).map((host, index) => Object.assign(host, {
        _id: generateId(),
        [metaField]: index,
        [timeField]: ISODate(`20${index}0-01-01`),
    }));
}

function verifyBucketsOnShard(shard, expectedBuckets) {
    const buckets =
        shard.getDB(dbName).getCollection(`system.buckets.${collName}`).find({}).toArray();
    assert.eq(buckets.length, expectedBuckets.length, tojson(buckets));

    const usedBucketIds = new Set();
    for (const expectedBucket of expectedBuckets) {
        let found = false;
        for (const bucket of buckets) {
            if (!usedBucketIds.has(bucket._id) && expectedBucket.meta === bucket.meta &&
                expectedBucket.minTime.toString() === bucket.control.min[timeField].toString() &&
                expectedBucket.maxTime.toString() === bucket.control.max[timeField].toString()) {
                found = true;
                usedBucketIds.add(bucket._id);
                break;
            }
        }

        assert(
            found,
            "Failed to find bucket " + tojson(expectedBucket) + " in the list " + tojson(buckets));
    }
}

function runTest(getShardKey, insert) {
    assert.commandWorked(mainDB.createCollection(
        collName, {timeseries: {timeField: timeField, metaField: metaField}}));
    const coll = mainDB.getCollection(collName);

    // The 'isTimeseriesNamespace' parameter is not allowed on mongos.
    assert.commandFailedWithCode(mainDB.runCommand({
        insert: `system.buckets.${collName}`,
        documents: [{[timeField]: ISODate()}],
        isTimeseriesNamespace: true
    }),
                                 5916401);

    // On a mongod node, 'isTimeseriesNamespace' can only be used on time-series buckets namespace.
    assert.commandFailedWithCode(
        st.shard0.getDB(dbName).runCommand(
            {insert: collName, documents: [{[timeField]: ISODate()}], isTimeseriesNamespace: true}),
        5916400);

    // Shard timeseries collection.
    const shardKey = getShardKey(1, 1);
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
    }));

    // Insert initial set of documents.
    const numDocs = 4;
    const firstBatch = generateBatch(numDocs);
    assert.commandWorked(insert(coll, firstBatch));

    // Manually split the data into two chunks.
    const splitIndex = numDocs / 2;
    const splitPoint = {};
    if (shardKey.hasOwnProperty(metaField)) {
        splitPoint.meta = firstBatch[splitIndex][metaField];
    }
    if (shardKey.hasOwnProperty(timeField)) {
        splitPoint[`control.min.${timeField}`] = firstBatch[splitIndex][timeField];
    }

    assert.commandWorked(
        mongos.adminCommand({split: `${dbName}.system.buckets.${collName}`, middle: splitPoint}));

    // Ensure that currently both chunks reside on the primary shard.
    let counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    const primaryShard = st.getPrimaryShard(dbName);
    assert.eq(2, counts[primaryShard.shardName], counts);

    // Move one of the chunks into the second shard.
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(mongos.adminCommand({
        movechunk: `${dbName}.system.buckets.${collName}`,
        find: splitPoint,
        to: otherShard.name,
        _waitForDelete: true
    }));

    // Ensure that each shard owns one chunk.
    counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
    assert.eq(1, counts[primaryShard.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    // Ensure that each shard has only 2 buckets.
    const primaryBuckets = [];
    const otherBuckets = [];
    for (let index = 0; index < numDocs; index++) {
        const doc = firstBatch[index];
        const bucket = {
            meta: doc[metaField],
            minTime: doc[timeField],
            maxTime: doc[timeField],
        };

        if (index < splitIndex) {
            primaryBuckets.push(bucket);
        } else {
            otherBuckets.push(bucket);
        }
    }
    verifyBucketsOnShard(primaryShard, primaryBuckets);
    verifyBucketsOnShard(otherShard, otherBuckets);

    // Ensure that after chunk migration all documents are still available.
    assert.docEq(firstBatch, coll.find().sort({_id: 1}).toArray());

    // Insert more documents with the same meta value range. These inserts should create new buckets
    // because we cannot update any bucket after a chunk migration.
    const secondBatch = generateBatch(numDocs);
    assert.commandWorked(insert(coll, secondBatch));

    const thirdBatch = generateBatch(numDocs);
    assert.commandWorked(insert(coll, thirdBatch));

    // Primary shard should contain 4 (2 + 2) buckets.
    verifyBucketsOnShard(primaryShard, primaryBuckets.concat(primaryBuckets));

    // During chunk migration, we have moved 2 buckets into the other shard. These migrated buckets
    // cannot be modified, so after insertion of second and third batches, two more buckets are
    // created.
    verifyBucketsOnShard(otherShard, otherBuckets.concat(otherBuckets));

    // Check that both old documents and newly inserted documents are available.
    const allDocuments = firstBatch.concat(secondBatch).concat(thirdBatch);
    assert.docEq(allDocuments, coll.find().sort({_id: 1}).toArray());

    // Check queries with shard key.
    for (let index = 0; index < numDocs; index++) {
        const expectedDocuments = [firstBatch[index], secondBatch[index], thirdBatch[index]];
        const actualDocuments =
            coll.find(getShardKey(firstBatch[index][metaField], firstBatch[index][timeField]))
                .sort({_id: 1})
                .toArray();
        assert.docEq(expectedDocuments, actualDocuments);
    }

    assert(coll.drop());
}

try {
    TimeseriesTest.run((insert) => {
        function metaShardKey(meta, _) {
            return {[metaField]: meta};
        }
        runTest(metaShardKey, insert);

        function timeShardKey(_, time) {
            return {[timeField]: time};
        }
        runTest(timeShardKey, insert);

        function timeAndMetaShardKey(meta, time) {
            return {[metaField]: meta, [timeField]: time};
        }
        runTest(timeAndMetaShardKey, insert);
    }, mainDB);
} finally {
    st.stop();
}
})();
