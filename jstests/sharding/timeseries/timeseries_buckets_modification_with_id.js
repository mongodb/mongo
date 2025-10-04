/**
 * Tests deleteOne and updateOne works when invoked directly on the raw time-series buckets.
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

const mongos = st.s;
const testDB = mongos.getDB(jsTestName());
const collName = "ts";
const timeFieldName = "time";
const metaFieldName = "tag";
// {
//     "_id": ObjectId("64dd4ae9ac4fd7e3ebbd9af3"),
//     "control": {
//         "version": 1,
//         "min":
//             {"_id": ObjectId("64dd4ae9a2c44e75d1151285"), "time":
//             ISODate("2023-08-16T00:00:00Z")},
//         "max": {
//             "_id": ObjectId("64dd4ae9a2c44e75d1151285"),
//             "time": ISODate("2023-08-16T22:17:13.749Z")
//         }
//     },
//     "meta": 1,
//     "data": {
//         "_id": {"0": ObjectId("64dd4ae9a2c44e75d1151285")},
//         "time": {"0": ISODate("2023-08-16T22:17:13.749Z")}
//     }
// };
const compressedBucketDoc = {
    "_id": ObjectId("66fdde800c625dc44b9004d2"),
    "control": {
        "version": 2,
        "min": {"_id": ObjectId("66feda942c8fee9e3a54f3a1"), "time": ISODate("2024-10-03T00:00:00Z")},
        "max": {"_id": ObjectId("66feda942c8fee9e3a54f3a1"), "time": ISODate("2024-10-03T13:52:00Z")},
        "count": 1,
    },
    "meta": 1,
    "data": {"_id": BinData(7, "BwBm/tqULI/unjpU86EA"), "time": BinData(7, "CQAA3KZSkgEAAAA=")},
};

function runTest(cmd, validateFn) {
    const coll = testDB.getCollection(collName);
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {
            timeseries: {timeField: timeFieldName, metaField: metaFieldName, granularity: "hours"},
        }),
    );

    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
    assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {[metaFieldName]: 1}}));

    assert.commandWorked(
        testDB.adminCommand({split: getTimeseriesCollForDDLOps(testDB, coll).getFullName(), middle: {meta: 1}}),
    );
    assert.commandWorked(
        testDB.adminCommand({
            moveChunk: getTimeseriesCollForDDLOps(testDB, coll).getFullName(),
            find: {meta: 1},
            to: st.getOther(st.getPrimaryShard(testDB.getName())).shardName,
            _waitForDelete: true,
        }),
    );

    // Tests the command works for the unsharded collection.
    assert.commandWorked(
        getTimeseriesCollForRawOps(testDB, coll).insert(compressedBucketDoc, getRawOperationSpec(testDB)),
    );
    assert.commandWorked(testDB.runCommand(cmd));
    validateFn(coll);
}

function removeValidateFn(coll) {
    assert.eq(getTimeseriesCollForRawOps(testDB, coll).find().rawData().itcount(), 0);
}

function updateValidateFn(coll) {
    assert.eq(getTimeseriesCollForRawOps(testDB, coll).find({"control.closed": true}).rawData().itcount(), 1);
}

runTest(
    {
        delete: getTimeseriesCollForRawOps(testDB, collName),
        deletes: [{q: {_id: ObjectId("66fdde800c625dc44b9004d2")}, limit: 1}],
        ...getRawOperationSpec(testDB),
    },
    removeValidateFn,
);

runTest(
    {
        update: getTimeseriesCollForRawOps(testDB, collName),
        updates: [
            {
                q: {_id: ObjectId("66fdde800c625dc44b9004d2")},
                u: {$set: {"control.closed": true}},
                multi: false,
            },
        ],
        ...getRawOperationSpec(testDB),
    },
    updateValidateFn,
);

st.stop();
