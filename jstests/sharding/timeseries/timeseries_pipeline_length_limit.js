/**
 * Tests that the aggregation pipeline length limit is enforced for timeseries collections after
 * translations and optimizations.
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const timeFieldName = "time";
const metaFieldName = "status";
const pipelineLengthLimit = 50;
const kPreParseErrCode = 7749501;
const kPostParseErrCode = 5054701;
const st = new ShardingTest({
    shards: 2,
    other: {
        mongosOptions: {setParameter: {internalPipelineLengthLimit: pipelineLengthLimit}},
        rsOptions: {setParameter: {internalPipelineLengthLimit: pipelineLengthLimit}},
    },
});
assert.commandWorked(st.s0.adminCommand({enablesharding: dbName}));

// Create a timeseries collection with a few documents, so the aggregation can do some work
// and avoid any optimizations where we might terminate early for empty collections.
const mongosDB = st.s0.getDB(dbName);
const tsColl = mongosDB.getCollection(jsTestName());
assertDropCollection(mongosDB, tsColl.getName());
assert.commandWorked(
    mongosDB.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);
const documents = [
    {_id: 0, [timeFieldName]: new Date("2021-09-30T07:46:38.746Z"), [metaFieldName]: 2, a: 5},
    {_id: 1, [timeFieldName]: new Date("2021-09-30T08:15:38.000Z"), [metaFieldName]: 1, a: 5, b: 8},
    {
        _id: 2,
        [timeFieldName]: new Date("2021-09-30T08:45:38.000Z"),
        [metaFieldName]: 1,
        a: 7,
        b: 12,
    },
    {_id: 3, [timeFieldName]: new Date("2021-09-30T08:15:38.000Z"), [metaFieldName]: 2, a: 2, b: 7},
    {
        _id: 4,
        [timeFieldName]: new Date("2021-09-30T09:23:38.000Z"),
        [metaFieldName]: 4,
        a: 8,
        b: 10,
    },
];
assert.commandWorked(tsColl.insertMany(documents));

function testLimits(testDB, isMongos = true) {
    // An empty pipeline over a timeseries collection should return all documents. The collection is
    // unsharded and might be on any shard, so we shouldn't run this test if we are connected to a
    // shard.
    if (isMongos) {
        let results = tsColl.aggregate([]).toArray();
        assert.sameMembers(results, documents);
    }

    const stages = [{$addFields: {c: 5}}, {$project: {a: 1, [timeFieldName]: 1, [metaFieldName]: 1}}];
    let pipeline = [];

    // Validate a pipeline of length 'pipelineLengthLimit - 1' succeeds. The pipeline will be at the
    // maximum length because queries on timeseries add an '$_internalUnpackBucket' stage.
    for (let i = 0; i < pipelineLengthLimit - 1; i++) {
        pipeline.push(stages[i % stages.length]);
    }
    assert.commandWorked(testDB.runCommand({aggregate: tsColl.getName(), pipeline: pipeline, cursor: {}}));

    // Add a $limit stage to the front of the pipeline. The pipeline should fail, since the pipeline
    // exceeds the length limit with the '$_internalUnpackBucket' stage.
    pipeline.unshift({$limit: 3});
    assert.commandFailedWithCode(
        assert.throws(() => tsColl.aggregate(pipeline).toArray()),
        [kPreParseErrCode, kPostParseErrCode],
    );

    // Remove the last stage in the pipeline. The pipeline should fail after optimizing. There is an
    // optimization in '$_internalUnpackBucket' to push down a new $limit stage. With the additional
    // $limit stage, the pipeline exceeds the length limit.
    pipeline.pop();
    assert.commandFailedWithCode(
        assert.throws(() => tsColl.aggregate(pipeline).toArray()),
        [kPostParseErrCode],
    );
}

// Run test against mongos.
testLimits(mongosDB);

// Run test against shard.
testLimits(st.rs0.getPrimary().getDB(dbName), false /* isMongos */);

st.stop();
