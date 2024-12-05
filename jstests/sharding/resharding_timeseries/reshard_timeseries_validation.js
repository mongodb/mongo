// Basic validation tests for resharding timeseries collections.
// @tags: [
//   requires_fcv_80,
// ]
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const kCollName = 'foo';
const ns = kDbName + '.' + kCollName;
const mongos = st.s0;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

const timeFieldName = 'time';
const metaFieldName = 'hostId';

const timeseriesOptions = {
    timeField: timeFieldName,
    metaField: metaFieldName,
};

assert.commandWorked(st.s.adminCommand({
    shardCollection: ns,
    key: {[metaFieldName]: 1},
    timeseries: timeseriesOptions,
}));

const kBucketCollName = "system.buckets.foo";
const kBucketNss = kDbName + "." + kBucketCollName;

let timeseriesCollDoc = st.config.collections.findOne({_id: kBucketNss});
assert.eq(timeseriesCollDoc.timeseriesFields.timeField, timeseriesOptions.timeField);
assert.eq(timeseriesCollDoc.timeseriesFields.metaField, timeseriesOptions.metaField);
assert.eq(timeseriesCollDoc.key, {meta: 1});

const sDB = st.s.getDB(kDbName);

// Insert some docs
assert.commandWorked(sDB.getCollection(kCollName).insert([
    {data: 1, time: new Date(), hostId: {x: 1, y: 5}},
    {data: 3, time: new Date(), hostId: {x: 2, y: 3}},
    {data: 3, time: new Date(), hostId: {x: 3, y: 2}},
    {data: 1, time: new Date(), hostId: {x: 4, y: 1}}
]));

// Failure scenarios.
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {_id: 1}}),
                             [5914001]);
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {a: 1}}), [5914001]);
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {[timeFieldName]: 1, [metaFieldName]: 1}}),
    [5914000]);
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {[timeFieldName]: 'hashed'}}), [880031]);

function reshardAndVerifyShardKeyAndIndexes(
    newKey, indexIdx, expectedViewIndexKey, expectedBucketIndexKey, expectedBucketShardKey) {
    jsTestLog("Resharding to new key:");
    printjson(newKey);

    assert.commandWorked(mongos.adminCommand({
        reshardCollection: ns,
        key: newKey,
        numInitialChunks: 1,
    }));

    const viewIndexes =
        assert.commandWorked(sDB.getCollection(kCollName).runCommand({listIndexes: kCollName}));
    assert.eq(viewIndexes.cursor.firstBatch[indexIdx]["key"], expectedViewIndexKey);

    const bucketIndexes = assert.commandWorked(
        sDB.getCollection(kBucketCollName).runCommand({listIndexes: kBucketCollName}));
    assert.eq(bucketIndexes.cursor.firstBatch[indexIdx]["key"], expectedBucketIndexKey);

    let configCollectionsBucketsEntry = st.config.collections.findOne({_id: kBucketNss});
    assert.eq(configCollectionsBucketsEntry["key"], expectedBucketShardKey);
}

// Success scenarios.
reshardAndVerifyShardKeyAndIndexes({[timeFieldName]: 1},
                                   1,
                                   {[timeFieldName]: 1},
                                   {"control.min.time": 1, "control.max.time": 1},
                                   {"control.min.time": 1});
reshardAndVerifyShardKeyAndIndexes(
    {'hostId.x': "hashed"}, 2, {"hostId.x": "hashed"}, {"meta.x": "hashed"}, {"meta.x": "hashed"});
reshardAndVerifyShardKeyAndIndexes({[metaFieldName]: 1},
                                   0,
                                   {[metaFieldName]: 1, [timeFieldName]: 1},
                                   {"meta": 1, "control.min.time": 1, "control.max.time": 1},
                                   {"meta": 1});
reshardAndVerifyShardKeyAndIndexes({'hostId.y': 1, [timeFieldName]: 1},
                                   3,
                                   {"hostId.y": 1, [timeFieldName]: 1},
                                   {"meta.y": 1, "control.min.time": 1, "control.max.time": 1},
                                   {"meta.y": 1, "control.min.time": 1});

st.stop();
