/**
 * Tests passing a hint to the findAndModify command on a time-series collection for updates.
 * @tags: [
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # Retryable findandmodify is not supported on timeseries collections
 *   does_not_support_retryable_writes
 * ]
 */

import {getWinningPlanFromExplain} from 'jstests/libs/analyze_plan.js';

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-01-01T18:00:00Z");
const collName = "t";
const dbName = jsTestName();

const testUpdateHint = ({
    initialDocList,
    indexes,
    query,
    update,
    hint,
    resultDocList,
    nModifiedBuckets,
    expectedPlan,
    expectedError
}) => {
    const testDB = db.getSiblingDB(dbName);
    const coll = testDB.getCollection(collName);

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(coll.createIndexes(indexes));

    assert.commandWorked(coll.insert(initialDocList));

    const findAndModifyCmd =
        {findAndModify: coll.getName(), query: query, update: update, hint: hint};

    if (expectedError != undefined) {
        assert.commandFailedWithCode(
            testDB.runCommand(
                {findAndModify: coll.getName(), query: query, update: update, hint: hint}),
            expectedError);

        assert.eq(initialDocList.length, resultDocList.length);

        resultDocList.forEach(resultDoc => {
            const actualDoc = coll.findOne(resultDoc);
            assert(actualDoc,
                   "Document " + tojson(resultDoc) +
                       " is not found in the result collection as expected ");
            assert.docEq(resultDoc, actualDoc);
        });
    } else {
        const res = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
        assert.eq(nModifiedBuckets, res.lastErrorObject.n);
        assert.eq(initialDocList.length, resultDocList.length);

        resultDocList.forEach(resultDoc => {
            const actualDoc = coll.findOne(resultDoc);
            assert(actualDoc,
                   "Document " + tojson(resultDoc) +
                       " is not found in the result collection as expected ");
            assert.docEq(resultDoc, actualDoc);
        });

        const winningPlan = getWinningPlanFromExplain(assert.commandWorked(
            testDB.runCommand({explain: findAndModifyCmd, verbosity: "executionStats"})));

        // Verify that the query plan uses the expected index.
        if (expectedPlan.stage == "COLLSCAN") {
            assert.eq(expectedPlan.stage, winningPlan.inputStage.stage);
        } else {
            assert.eq(expectedPlan.stage, winningPlan.inputStage.inputStage.stage);
            assert.eq(bsonWoCompare(expectedPlan.keyPattern,
                                    winningPlan.inputStage.inputStage.keyPattern),
                      0);
        }
    }

    assert(coll.drop());
};

const hintDoc1 = {
    _id: 1,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 1},
    measurement: 1
};
const hintDoc2 = {
    _id: 2,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 2},
    measurement: 2
};
const hintDoc3 = {
    _id: 3,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 3},
    measurement: 3
};

/************* Tests passing a hint to an update on a collection with a single index. *************/
// Query on and update the measurement field using a forward collection scan: hint: {$natural 1}.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1}],
    query: {[metaFieldName + '.a']: {$gte: 3}},
    update: {$inc: {measurement: 10}},
    hint: {$natural: 1},
    resultDocList: [
        hintDoc1,
        hintDoc2,
        {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: {"a": 3}, measurement: 13}
    ],
    nModifiedBuckets: 1,
    expectedPlan: {stage: "COLLSCAN"}
});

// Query on and update the measurement field using a backward collection scan: hint: {$natural -1}.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1}],
    query: {[metaFieldName + '.a']: {$gte: 3}},
    update: {$inc: {measurement: 10}},
    hint: {$natural: -1},
    resultDocList: [
        hintDoc1,
        hintDoc2,
        {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: {"a": 3}, measurement: 13}
    ],
    nModifiedBuckets: 1,
    expectedPlan: {stage: "COLLSCAN"}
});

// Query on and update a measurement using the metaField index as a hint, specifying the hint with
// an index specification document.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1}],
    query: {[metaFieldName + '.a']: {$lte: 1}},
    update: {$inc: {measurement: 10}},
    hint: {[metaFieldName]: 1},
    resultDocList: [
        {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {"a": 1}, measurement: 11},
        hintDoc2,
        hintDoc3
    ],
    nModifiedBuckets: 1,
    expectedPlan: {stage: "IXSCAN", keyPattern: {"meta": 1}},
});

// Query on and update a measurement using a compound index on the metaField and timeField as a
// hint, specifying the hint with an index specification document.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1, [timeFieldName]: 1}],
    query: {[metaFieldName]: {a: 2}},
    update: {$inc: {measurement: 10}},
    hint: {[metaFieldName]: 1, [timeFieldName]: 1},
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 2}, measurement: 12},
        hintDoc3
    ],
    nModifiedBuckets: 1,
    expectedPlan:
        {stage: "IXSCAN", keyPattern: {"meta": 1, "control.min.time": 1, "control.max.time": 1}},
});

testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: -1, [timeFieldName]: 1}],
    query: {[metaFieldName]: {a: 2}},
    update: {$inc: {measurement: 10}},
    hint: {[metaFieldName]: -1, [timeFieldName]: 1},
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 2}, measurement: 12},
        hintDoc3
    ],
    nModifiedBuckets: 1,
    expectedPlan:
        {stage: "IXSCAN", keyPattern: {"meta": -1, "control.min.time": 1, "control.max.time": 1}},
});

// Query on and update a measurement using a compound index on the metaField and timeField as a
// hint, specifying the hint with the index name string.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1, [timeFieldName]: 1}],
    query: {[metaFieldName]: {a: 2}},
    update: {$inc: {measurement: 10}},
    hint: metaFieldName + "_1_" + timeFieldName + "_1",
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 2}, measurement: 12},
        hintDoc3
    ],
    nModifiedBuckets: 1,
    expectedPlan:
        {stage: "IXSCAN", keyPattern: {"meta": 1, "control.min.time": 1, "control.max.time": 1}},
});

// Query on and update a measurement using a compound index on the timeField and an embedded field
// of the metaField as hints, specifying the hint with the index name string.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: -1, [timeFieldName]: 1}],
    query: {[metaFieldName]: {a: 2}},
    update: {$set: {measurement: 10}},
    hint: metaFieldName + ".a_-1_" + timeFieldName + "_1",
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 2}, measurement: 10},
        hintDoc3,
    ],
    nModifiedBuckets: 1,
    expectedPlan:
        {stage: "IXSCAN", keyPattern: {"meta.a": -1, "control.min.time": 1, "control.max.time": 1}},
});

/************ Tests passing a hint to an update on a collection with multiple indexes. ************/
// Query on and update measurement of a collection with multiple indexes using the timeField index
// as a hint.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: 1}, {[timeFieldName]: -1}],
    query: {[metaFieldName]: {a: 1}},
    update: {$set: {measurement: 10}},
    hint: {[timeFieldName]: -1},
    resultDocList: [
        {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {"a": 1}, measurement: 10},
        hintDoc2,
        hintDoc3
    ],
    nModifiedBuckets: 1,
    expectedPlan: {stage: "IXSCAN", keyPattern: {"control.max.time": -1, "control.min.time": -1}},
});

// A non-existent index specified with an index specification document should fail.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: 1}, {[timeFieldName]: -1}],
    query: {[metaFieldName]: {a: 1}},
    update: {$set: {measurement: 10}},
    hint: {"badIndexName": -1},
    resultDocList: [hintDoc1, hintDoc2, hintDoc3],
    expectedPlan: {},
    expectedError: ErrorCodes.BadValue,
});

// A non-existent index specified with a string should fail.
testUpdateHint({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: 1}, {[timeFieldName]: -1}],
    query: {[metaFieldName]: {a: 1}},
    update: {$set: {measurement: 10}},
    hint: "bad_index_name",
    resultDocList: [hintDoc1, hintDoc2, hintDoc3],
    expectedPlan: {},
    expectedError: ErrorCodes.BadValue,
});
