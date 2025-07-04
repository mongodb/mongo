/**
 * Tests that the $indexstats aggregation pipeline returns timeseries index statistics.
 *
 * @tags: [
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {
    createRawTimeseriesIndex,
    getRawOperationSpec,
    getTimeseriesCollForRawOps
} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const dbName = 'testDB';
const collName = 'testColl';
const mongosDB = st.s.getDB(dbName);
const mongosColl = mongosDB.getCollection(collName);
const timeField = 'tm';
const metaField = 'mt';

// Create a timeseries collection.
assert.commandWorked(mongosDB.createCollection(
    collName, {timeseries: {timeField: timeField, metaField: metaField}}));
// Create an index over a time-series measurement field.
assert.commandWorked(mongosColl.createIndex({[metaField]: 1}));
// Create an index directly over the raw buckets.
assert.commandWorked(createRawTimeseriesIndex(mongosColl, {'control.time.min': 1}));

// Insert some documents.
const numberDoc = 20;
for (let i = 0; i < numberDoc; i++) {
    assert.commandWorked(mongosColl.insert({[timeField]: ISODate(), [metaField]: i}));
}
assert.eq(mongosColl.count(), numberDoc);

function checkIndexStats(coll, keys, sharded, rawOpSpec = {}) {
    if (sharded) {
        const shardedKeys = [];
        keys.forEach(x => {
            shardedKeys.push(x);
            shardedKeys.push(x);
        });
        keys = shardedKeys;
    }
    // Keep the index in the same order by index name.
    keys = keys.sort((x, y) => Object.keys(x)[0] < Object.keys(y)[0] ? -1 : 1);
    let indices = coll.aggregate([{$indexStats: {}}], rawOpSpec)
                      .toArray()
                      .sort((x, y) => x.name < y.name ? -1 : 1);
    assert.eq(indices.length,
              keys.length,
              `There should be ${keys.length} indices on the collection.\n${tojson(indices)}`);
    indices.forEach((index, i) => {
        assert(index.hasOwnProperty('shard'), tojson(index));
        assert.docEq(
            keys[i], index.key, `Index should have key spec ${tojson(keys[i])}.\n${tojson(index)}`);
    });
}

// Check indexStats before sharding.
checkIndexStats(mongosColl, [{[metaField]: 1, [timeField]: 1}, {[metaField]: 1}], false);
checkIndexStats(
    getTimeseriesCollForRawOps(mongosDB, mongosColl),
    [{"meta": 1, "control.min.tm": 1, "control.max.tm": 1}, {"meta": 1}, {"control.time.min": 1}],
    false /*sharded*/,
    getRawOperationSpec(mongosColl.getDB()));

// Shard the timeseries collection.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({
    shardCollection: mongosColl.getFullName(),
    key: {[metaField]: 1},
}));

const primaryShard = st.getPrimaryShard(dbName);
const otherShard = st.getOther(primaryShard);
const splitPoint = {
    meta: numberDoc / 2
};
assert.commandWorked(st.s.adminCommand(
    {split: getTimeseriesCollForDDLOps(mongosDB, mongosColl).getFullName(), middle: splitPoint}));
// Ensure that currently both chunks reside on the primary shard.
let counts = st.chunkCounts(collName, dbName);
assert.eq(2, counts[primaryShard.shardName]);
// Move one of the chunks into the second shard.
assert.commandWorked(st.s.adminCommand({
    movechunk: getTimeseriesCollForDDLOps(mongosDB, mongosColl).getFullName(),
    find: splitPoint,
    to: otherShard.name,
    _waitForDelete: true
}));
// Ensure that each shard owns one chunk.
counts = st.chunkCounts(collName, dbName);
assert.eq(1, counts[primaryShard.shardName], counts);
assert.eq(1, counts[otherShard.shardName], counts);

// Create an index after sharding.
assert.commandWorked(createRawTimeseriesIndex(mongosColl, {'control.time.max': 1}));

// Check indexStats after sharding.
checkIndexStats(mongosColl, [{[metaField]: 1, [timeField]: 1}, {[metaField]: 1}], true);
checkIndexStats(getTimeseriesCollForRawOps(mongosDB, mongosColl),
                [
                    {"meta": 1, "control.min.tm": 1, "control.max.tm": 1},
                    {"meta": 1},
                    {"control.time.min": 1},
                    {"control.time.max": 1}
                ],
                true /*sharded*/,
                getRawOperationSpec(mongosColl.getDB()));

st.stop();
