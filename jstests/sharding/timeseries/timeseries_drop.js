/**
 * Test drop of time-series collection.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
    getTimeseriesCollForDDLOps
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'hostid';

// Connections.
const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const mongos = st.s0;

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

function ensureCollectionExists(collName, db) {
    const collections = db.getCollectionNames();
    assert(collections.includes(collName), collections);
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
        shardCollection: coll.getFullName(),
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
            {split: getTimeseriesCollForDDLOps(mainDB, coll).getFullName(), middle: splitPoint}));

        // Ensure that currently both chunks reside on the primary shard.
        let counts = st.chunkCounts(collName, dbName);
        const primaryShard = st.getPrimaryShard(dbName);
        assert.eq(2, counts[primaryShard.shardName], counts);

        // Move one of the chunks into the second shard.
        const otherShard = st.getOther(primaryShard);
        assert.commandWorked(mongos.adminCommand({
            movechunk: getTimeseriesCollForDDLOps(mainDB, coll).getFullName(),
            find: splitPoint,
            to: otherShard.name,
            _waitForDelete: true
        }));

        // Ensure that each shard owns one chunk.
        counts = st.chunkCounts(collName, dbName);
        assert.eq(1, counts[primaryShard.shardName], counts);
        assert.eq(1, counts[otherShard.shardName], counts);
    }

    // TODO SERVER-101784 remove this check once only viewless timeseries exist.
    // TODO SERVER-107138 update once drop on the buckets namespace fails on FCV 9.0.
    if (!areViewlessTimeseriesEnabled(mainDB)) {
        // Confirm it's illegal to directly drop the time-series buckets collection.
        assert.commandFailedWithCode(mainDB.runCommand({drop: getTimeseriesBucketsColl(collName)}),
                                     ErrorCodes.IllegalOperation);
        ensureCollectionExists(collName, mainDB);
        ensureCollectionExists(getTimeseriesBucketsColl(collName), mainDB);
    }

    // Drop the time-series collection.
    assert(coll.drop());

    // Ensure that the time-series collection doesn't exist according to mongos and shards.
    ensureCollectionDoesNotExist(collName);

    // TODO SERVER-101784 remove this check once only viewless timeseries exist.
    // Ensure that the time-series buckets collection doesn't exist according to mongos and shards.
    ensureCollectionDoesNotExist(getTimeseriesBucketsColl(collName));

    // Ensure that the time-series collection gets deleted from the config database.
    assert.eq(
        [],
        configDB.collections.find({_id: getTimeseriesCollForDDLOps(mainDB, coll).getFullName()})
            .toArray());
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
