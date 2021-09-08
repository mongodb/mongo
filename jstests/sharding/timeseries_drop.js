/**
 * Test drop of time-series collection.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_find_command
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

// Databases.
const mainDB = mongos.getDB(dbName);
const configDB = mongos.getDB('config');

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

function ensureCollectionDoesNotExist(collName) {
    const databases = [mainDB, st.shard0.getDB(dbName), st.shard1.getDB(dbName)];
    for (const db of databases) {
        const collections = db.getCollectionNames();
        assert(!collections.includes(collName), collections);
    }
}

function runTest(getShardKey, performChunkSplit) {
    mainDB.dropDatabase();

    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

    // Create timeseries collection.
    assert.commandWorked(mainDB.createCollection(
        collName, {timeseries: {timeField: timeField, metaField: metaField}}));
    const coll = mainDB.getCollection(collName);

    // Shard timeseries collection.
    const shardKey = getShardKey(1, 1);
    assert.commandWorked(coll.createIndex(shardKey));
    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
    }));

    // Insert initial set of documents.
    const numDocs = 8;
    const firstBatch = generateBatch(numDocs);
    assert.commandWorked(coll.insert(firstBatch));

    if (performChunkSplit) {
        // Manually split the data into two chunks.
        const splitIndex = numDocs / 2;
        const splitPoint = {};
        if (shardKey.hasOwnProperty(metaField)) {
            splitPoint.meta = firstBatch[splitIndex][metaField];
        }
        if (shardKey.hasOwnProperty(timeField)) {
            splitPoint[`control.min.${timeField}`] = firstBatch[splitIndex][timeField];
        }

        assert.commandWorked(mongos.adminCommand(
            {split: `${dbName}.system.buckets.${collName}`, middle: splitPoint}));

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
    }

    // Drop the time-series collection.
    assert(coll.drop());

    // Ensure that both time-series view and time-series buckets collections do not exist according
    // to mongos and both shards.
    ensureCollectionDoesNotExist(collName);
    ensureCollectionDoesNotExist(`system.buckets.${collName}`);

    // Ensure that the time-series buckets collection gets deleted from the config database as well.
    assert.eq([],
              configDB.collections.find({_id: `${dbName}.system.buckets.${collName}`}).toArray());
}

try {
    for (let performChunkSplit of [false, true]) {
        function metaShardKey(meta, _) {
            return {[metaField]: meta};
        }
        runTest(metaShardKey, performChunkSplit);

        function timeShardKey(_, time) {
            return {[timeField]: time};
        }
        runTest(timeShardKey, performChunkSplit);

        function timeAndMetaShardKey(meta, time) {
            return {[metaField]: meta, [timeField]: time};
        }
        runTest(timeAndMetaShardKey, performChunkSplit);
    }
} finally {
    st.stop();
}
})();
