/**
 * Tests passing a hint to the findAndModify command on a time-series collection for deletes.
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
import {getWinningPlanFromExplain} from 'jstests/libs/query/analyze_plan.js';

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-01-01T18:00:00Z");
const collName = "t";
const dbName = jsTestName();
const collNamespace = dbName + '.' + collName;
const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const testDeleteHint = ({
    docsToInsert,
    expectedRemainingDocs,
    expectedNRemoved,
    deleteQuery,
    hint,
    indexes,
    expectedPlan,
    expectedError
}) => {
    const testDB = db.getSiblingDB(dbName);
    const coll = testDB.getCollection(collName);
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(coll.createIndexes(indexes));
    assert.commandWorked(coll.insert(docsToInsert));

    const findAndModifyCmd =
        {findAndModify: coll.getName(), query: deleteQuery, remove: true, hint: hint};

    if (expectedError != undefined) {
        assert.commandFailedWithCode(testDB.runCommand(findAndModifyCmd), expectedError);

        assert.eq(docsToInsert.length, expectedRemainingDocs.length);

        expectedRemainingDocs.forEach(resultDoc => {
            const actualDoc = coll.findOne(resultDoc);
            assert(actualDoc,
                   "Document " + tojson(resultDoc) +
                       " is not found in the result collection as expected ");
            assert.docEq(resultDoc, actualDoc);
        });
    } else {
        const res = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
        assert.eq(res.lastErrorObject.n, expectedNRemoved);
        assert.sameMembers(expectedRemainingDocs, coll.find({}).toArray());

        const winningPlan = getWinningPlanFromExplain(assert.commandWorked(
            testDB.runCommand({explain: findAndModifyCmd, verbosity: "executionStats"})));

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

const objA = {
    _id: 1,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 1},
    measurement: 1
};
const objB = {
    _id: 2,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 2},
    measurement: 2
};
const objC = {
    _id: 3,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 3},
    measurement: 3
};

// Query using a $natural hint.
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objB, objC],
    expectedNRemoved: 1,
    deleteQuery: {[metaFieldName]: {a: 1}},
    hint: {$natural: 1},
    indexes: [{[metaFieldName]: 1}],
    expectedPlan: {stage: "COLLSCAN"}
});

testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objB, objC],
    expectedNRemoved: 1,
    deleteQuery: {[metaFieldName]: {a: 1}},
    hint: {$natural: -1},
    indexes: [{[metaFieldName]: 1}],
    expectedPlan: {stage: "COLLSCAN"}
});

// Query using the metaField index as a hint.
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objB, objC],
    expectedNRemoved: 1,
    deleteQuery: {[metaFieldName]: {a: 1}},
    hint: {[metaFieldName]: 1},
    indexes: [{[metaFieldName]: 1}],
    expectedPlan: {stage: "IXSCAN", keyPattern: {"meta": 1}}
});

// // Query on a collection with a compound index using the timeField and metaField indexes as
// // hints.
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objB, objC],
    expectedNRemoved: 1,
    deleteQuery: {[metaFieldName]: {a: 1}},
    hint: {[timeFieldName]: 1, [metaFieldName]: 1},
    indexes: [{[timeFieldName]: 1, [metaFieldName]: 1}],
    expectedPlan:
        {stage: "IXSCAN", keyPattern: {"control.min.time": 1, "control.max.time": 1, "meta": 1}}
});

// Query on a collection with a compound index using the timeField and metaField indexes as
// hints
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objA, objC],
    expectedNRemoved: 1,
    deleteQuery: {[metaFieldName]: {a: 2}},
    hint: {[timeFieldName]: -1, [metaFieldName]: 1},
    indexes: [{[timeFieldName]: -1, [metaFieldName]: 1}],
    expectedPlan:
        {stage: "IXSCAN", keyPattern: {"control.max.time": -1, "control.min.time": -1, "meta": 1}}
});

// // Query on a collection with a compound index using the timeField and metaField index names
// // as hints.
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objA, objC],
    expectedNRemoved: 1,
    deleteQuery: {[metaFieldName]: {a: 2}},
    hint: timeFieldName + "_1_" + metaFieldName + "_1",
    indexes: [{[timeFieldName]: 1, [metaFieldName]: 1}],
    expectedPlan:
        {stage: "IXSCAN", keyPattern: {"control.min.time": 1, "control.max.time": 1, "meta": 1}}
});

// Query on a collection with multiple indexes using the timeField index as a hint.
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objA, objB],
    expectedNRemoved: 1,
    deleteQuery: {[metaFieldName]: {a: 3}},
    hint: {[timeFieldName]: 1},
    indexes: [{[metaFieldName]: -1}, {[timeFieldName]: 1}],
    expectedPlan: {stage: "IXSCAN", keyPattern: {"control.min.time": 1, "control.max.time": 1}}
});

// Query on a collection with multiple indexes using an invalid index name.
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objA, objB, objC],
    expectedNRemoved: 0,
    deleteQuery: {[metaFieldName]: {a: 3}},
    hint: "test_hint",
    indexes: [{[metaFieldName]: -1}, {[timeFieldName]: 1}],
    expectedPlan: {},
    expectedError: ErrorCodes.BadValue,
});

// Query on a collection with multiple indexes using an invalid index spec.
testDeleteHint({
    docsToInsert: [objA, objB, objC],
    expectedRemainingDocs: [objA, objB, objC],
    expectedNRemoved: 0,
    deleteQuery: {[metaFieldName]: {a: 3}},
    hint: {"test_hint": 1},
    indexes: [{[timeFieldName]: 1, [metaFieldName]: 1}],
    expectedPlan: {},
    expectedError: ErrorCodes.BadValue
});
