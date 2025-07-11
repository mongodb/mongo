// Basic validation tests for resharding timeseries collections.
// @tags: [
//   requires_fcv_80,
// ]
//
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
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

const sDB = st.s.getDB(kDbName);
const sColl = sDB.getCollection(kCollName);

let timeseriesCollDoc =
    st.config.collections.findOne({_id: getTimeseriesCollForDDLOps(sDB, sColl).getFullName()});
assert.eq(timeseriesCollDoc.timeseriesFields.timeField, timeseriesOptions.timeField);
assert.eq(timeseriesCollDoc.timeseriesFields.metaField, timeseriesOptions.metaField);
assert.eq(timeseriesCollDoc.key, {meta: 1});

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

function reshardAndVerifyShardKeyAndIndexes(newKey,
                                            indexIdx,
                                            expectedIndexKey,
                                            expectedRawIndexKey,
                                            expectedRawShardKey,
                                            isShardKeyHashedPrefix) {
    jsTestLog("Resharding to new key:");
    printjson(newKey);

    let cmdObj = {reshardCollection: ns, key: newKey};
    if (!isShardKeyHashedPrefix) {
        cmdObj.numInitialChunks = 1;
    }
    assert.commandWorked(mongos.adminCommand(cmdObj));

    const collIndexes = assert.commandWorked(sColl.runCommand({listIndexes: kCollName}));
    assert.eq(collIndexes.cursor.firstBatch[indexIdx]["key"], expectedIndexKey);

    const rawCollIndexes = assert.commandWorked(getTimeseriesCollForRawOps(sDB, sColl).runCommand({
        listIndexes: getTimeseriesCollForRawOps(sDB, sColl).getName(),
        ...getRawOperationSpec(sDB)
    }));
    assert.eq(rawCollIndexes.cursor.firstBatch[indexIdx]["key"], expectedRawIndexKey);

    let configCollectionsEntry =
        st.config.collections.findOne({_id: getTimeseriesCollForDDLOps(sDB, sColl).getFullName()});
    assert.eq(configCollectionsEntry["key"], expectedRawShardKey);
}

// Success scenarios.
reshardAndVerifyShardKeyAndIndexes({[timeFieldName]: 1},
                                   1,
                                   {[timeFieldName]: 1},
                                   {"control.min.time": 1, "control.max.time": 1},
                                   {"control.min.time": 1},
                                   false /* isShardKeyHashedPrefix */);
reshardAndVerifyShardKeyAndIndexes({'hostId.x': "hashed"},
                                   2,
                                   {"hostId.x": "hashed"},
                                   {"meta.x": "hashed"},
                                   {"meta.x": "hashed"},
                                   true /* isShardKeyHashedPrefix */);
reshardAndVerifyShardKeyAndIndexes({[metaFieldName]: 1},
                                   0,
                                   {[metaFieldName]: 1, [timeFieldName]: 1},
                                   {"meta": 1, "control.min.time": 1, "control.max.time": 1},
                                   {"meta": 1},
                                   false /* isShardKeyHashedPrefix */);
reshardAndVerifyShardKeyAndIndexes({'hostId.y': 1, [timeFieldName]: 1},
                                   3,
                                   {"hostId.y": 1, [timeFieldName]: 1},
                                   {"meta.y": 1, "control.min.time": 1, "control.max.time": 1},
                                   {"meta.y": 1, "control.min.time": 1},
                                   false /* isShardKeyHashedPrefix */);

st.stop();
