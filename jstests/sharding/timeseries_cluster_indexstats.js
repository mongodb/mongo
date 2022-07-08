/**
 * Tests that the $indexstats aggregation pipeline returns timeseries index statistics.
 *
 * @tags: [
 *   requires_fcv_51,
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
load("jstests/core/timeseries/libs/timeseries.js");

const st = new ShardingTest({shards: 2});

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

const dbName = 'testDB';
const collName = 'testColl';
const mongosDB = st.s.getDB(dbName);
const mongosColl = mongosDB.getCollection(collName);
const mongosBucketColl = mongosDB.getCollection(`system.buckets.${collName}`);
const timeField = 'tm';
const metaField = 'mt';
const viewNs = `${dbName}.${collName}`;
const bucketNs = `${dbName}.system.buckets.${collName}`;

// Create a timeseries collection.
assert.commandWorked(mongosDB.createCollection(
    collName, {timeseries: {timeField: timeField, metaField: metaField}}));
// Create an index on the view namespace.
assert.commandWorked(mongosColl.createIndex({[metaField]: 1}));
// Create an index on the bucket namespace, this is invisible through view namespace.
assert.commandWorked(mongosBucketColl.createIndex({'control.time.min': 1}));

// Insert some documents.
const numberDoc = 20;
for (let i = 0; i < numberDoc; i++) {
    assert.commandWorked(mongosColl.insert({[timeField]: ISODate(), [metaField]: i}));
}
assert.eq(mongosColl.count(), numberDoc);

function checkIndexStats(coll, keys, sharded) {
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
    let indices =
        coll.aggregate([{$indexStats: {}}]).toArray().sort((x, y) => x.name < y.name ? -1 : 1);
    assert.eq(indices.length,
              keys.length,
              `There should be ${keys.length} indices on the collection.\n${tojson(indices)}`);
    indices.forEach((index, i) => {
        assert.eq(index.hasOwnProperty('shard'),
                  sharded,
                  sharded
                      ? `Index stats 'shard' field should exist on a sharded collection.\n${
                            tojson(index)}`
                      : `Index stats 'shard' field should not exist on a non-sharded collection.\n${
                            tojson(index)}`);
        assert.docEq(
            index.key, keys[i], `Index should have key spec ${tojson(keys[i])}.\n${tojson(index)}`);
    });
}

// Check indexStats before sharding.
if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(st.shard0)) {
    checkIndexStats(mongosColl, [{[metaField]: 1, [timeField]: 1}, {[metaField]: 1}], false);
    checkIndexStats(mongosBucketColl,
                    [
                        {"meta": 1, "control.min.tm": 1, "control.max.tm": 1},
                        {"meta": 1},
                        {"control.time.min": 1}
                    ],
                    false);
} else {
    checkIndexStats(mongosColl, [{[metaField]: 1}], false);
    checkIndexStats(mongosBucketColl, [{"meta": 1}, {"control.time.min": 1}], false);
}

// Shard the timeseries collection.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({
    shardCollection: viewNs,
    key: {[metaField]: 1},
}));

const primaryShard = st.getPrimaryShard(dbName);
const otherShard = st.getOther(primaryShard);
const splitPoint = {
    meta: numberDoc / 2
};
assert.commandWorked(st.s.adminCommand({split: bucketNs, middle: splitPoint}));
// Ensure that currently both chunks reside on the primary shard.
let counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
assert.eq(2, counts[primaryShard.shardName]);
// Move one of the chunks into the second shard.
assert.commandWorked(st.s.adminCommand(
    {movechunk: bucketNs, find: splitPoint, to: otherShard.name, _waitForDelete: true}));
// Ensure that each shard owns one chunk.
counts = st.chunkCounts(`system.buckets.${collName}`, dbName);
assert.eq(1, counts[primaryShard.shardName], counts);
assert.eq(1, counts[otherShard.shardName], counts);

// Create an index after sharding.
assert.commandWorked(mongosBucketColl.createIndex({'control.time.max': 1}));

// Check indexStats after sharding.
if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(st.shard0)) {
    checkIndexStats(mongosColl, [{[metaField]: 1, [timeField]: 1}, {[metaField]: 1}], true);
    checkIndexStats(mongosBucketColl,
                    [
                        {"meta": 1, "control.min.tm": 1, "control.max.tm": 1},
                        {"meta": 1},
                        {"control.time.min": 1},
                        {"control.time.max": 1}
                    ],
                    true);
} else {
    checkIndexStats(mongosColl, [{[metaField]: 1}], true);
    checkIndexStats(
        mongosBucketColl, [{"meta": 1}, {"control.time.min": 1}, {"control.time.max": 1}], true);
}

st.stop();
})();
