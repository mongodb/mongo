/**
 * Tests the fact that we round time values to the specified granularity before targeting shards.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_find_command,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/aggregation/extras/utils.js");

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';
const timeField = 'time';
const metaField = 'hostId';

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
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
const mainDB = mongos.getDB(dbName);

// Helpers.
let currentId = 0;
function generateId() {
    return currentId++;
}

function makeDocument(metaValue, timeValue) {
    return {
        _id: generateId(),
        [metaField]: metaValue,
        [timeField]: timeValue,
        data: Random.rand(),
    };
}

function getDocumentsFromShard(shard, id) {
    return shard.getDB(dbName)
        .getCollection(`system.buckets.${collName}`)
        .aggregate(
            [{$_unpackBucket: {timeField: timeField, metaField: metaField}}, {$match: {_id: id}}])
        .toArray();
}

function runTest() {
    // Create and shard timeseries collection.
    const shardKey = {[timeField]: 1};
    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
        timeseries: {timeField, metaField, granularity: "hours"}
    }));
    const coll = mainDB.getCollection(collName);

    // Insert initial set of documents.
    const timeValues = [
        ISODate("2000-01-01T00:00"),
        ISODate("2000-01-01T05:00"),
        ISODate("2000-01-01T15:00"),
        ISODate("2000-01-01T20:00")
    ];
    const documents = Array.from(timeValues, (time, index) => makeDocument(index, time));
    assert.commandWorked(coll.insert(documents));

    // Manually split the data into two chunks.
    const splitPoint = {[`control.min.${timeField}`]: ISODate("2000-01-01T10:30")};
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

    // Our sharded cluster looks like this:
    //
    // |             |  Primary shard  |   Other shard   |
    // |-------------|-----------------|-----------------|
    // | Chunk range | [MinKey, 10:30) | [10:30, MaxKey] |
    // | Documents   | 00:00, 05:00    | 15:00, 20:00    |
    //
    // Now, we will try to insert a document with time value 10:31. Since we have specified
    // granularity: "hours", the actual measurement inserted into bucket will have time value 10:00.
    // This means that it should be routed to the primary shard. If mongos does not round time value
    // before routing, our document would have been directed to the other shard, which is incorrect.
    const edgeDocument = makeDocument(0, ISODate("2000-01-01T10:31"));
    assert.commandWorked(coll.insert([edgeDocument]));
    documents.push(edgeDocument);

    assert.docEq([edgeDocument], getDocumentsFromShard(primaryShard, edgeDocument._id));
    assert.eq([], getDocumentsFromShard(otherShard, edgeDocument._id));

    const noFilterResult = coll.find({}).sort({_id: 1}).toArray();
    assert.docEq(documents, noFilterResult);

    for (let document of documents) {
        const result = coll.find({[timeField]: document[timeField]}).toArray();
        assert.docEq([document], result);
    }

    assert(coll.drop());
}

try {
    runTest();
} finally {
    st.stop();
}
})();
