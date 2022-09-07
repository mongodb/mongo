/**
 * Tests passing a hint to the update command on a time-series collection.
 * @tags: [
 *   # Fail points in this test do not exist on mongos.
 *   assumes_against_mongod_not_mongos,
 *   # $currentOp can't run with a readConcern other than 'local'.
 *   assumes_read_concern_unchanged,
 *   # This test only synchronizes updates on the primary.
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection, # TODO SERVER-60233: Remove this tag.
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Specifically testing multi-updates.
 *   requires_multi_updates,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # Test uses parallel shell to wait on fail point.
 *   uses_parallel_shell,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/curop_helpers.js");
load('jstests/libs/parallel_shell_helpers.js');

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-01-01T18:00:00Z");
const collName = "t";
const dbName = jsTestName();

/**
 * Confirms that the given update on the collection with the given indexes returns the expected set
 * of documents and uses the correct query plan.
 */
const testUpdateHintSucceeded =
    ({initialDocList, indexes, updateList, resultDocList, nModifiedBuckets, expectedPlan}) => {
        const testDB = db.getSiblingDB(dbName);
        const coll = testDB.getCollection(collName);

        const awaitTestUpdate = startParallelShell(funWithArgs(
            function(dbName,
                     collName,
                     timeFieldName,
                     metaFieldName,
                     initialDocList,
                     indexes,
                     updateList,
                     resultDocList,
                     nModifiedBuckets) {
                const testDB = db.getSiblingDB(dbName);
                const coll = testDB.getCollection(collName);

                assert.commandWorked(testDB.createCollection(
                    coll.getName(),
                    {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

                assert.commandWorked(coll.createIndexes(indexes));

                assert.commandWorked(coll.insert(initialDocList));

                assert.commandWorked(testDB.adminCommand(
                    {configureFailPoint: "hangAfterBatchUpdate", mode: "alwaysOn"}));

                const res = assert.commandWorked(
                    testDB.runCommand({update: coll.getName(), updates: updateList}));

                assert.eq(nModifiedBuckets, res.n);
                assert.eq(initialDocList.length, resultDocList.length);

                resultDocList.forEach(resultDoc => {
                    const actualDoc = coll.findOne(resultDoc);
                    assert(actualDoc,
                           "Document " + tojson(resultDoc) +
                               " is not found in the result collection as expected ");
                    assert.docEq(resultDoc, actualDoc);
                });

                assert(coll.drop());
            },
            dbName,
            collName,
            timeFieldName,
            metaFieldName,
            initialDocList,
            indexes,
            updateList,
            resultDocList,
            nModifiedBuckets));
        try {
            const childCurOp =
                waitForCurOpByFailPoint(testDB, coll.getFullName(), "hangAfterBatchUpdate")[0];

            // Verify that the query plan uses the expected index.
            assert.eq(childCurOp.planSummary, expectedPlan);
        } finally {
            assert.commandWorked(
                testDB.adminCommand({configureFailPoint: "hangAfterBatchUpdate", mode: "off"}));

            awaitTestUpdate();
        }
    };

/**
 * Confirms that the given update on the collection with the given indexes fails.
 */
function testUpdateHintFailed(
    {initialDocList, indexes, updateList, resultDocList, nModifiedBuckets, failCode}) {
    const testDB = db.getSiblingDB(dbName);
    const coll = testDB.getCollection(collName);

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(coll.createIndexes(indexes));

    assert.commandWorked(coll.insert(initialDocList));

    const res = assert.commandFailedWithCode(
        testDB.runCommand({update: coll.getName(), updates: updateList}), failCode);

    assert.eq(nModifiedBuckets, res.n);
    assert.eq(initialDocList.length, resultDocList.length);

    resultDocList.forEach(resultDoc => {
        const actualDoc = coll.findOne(resultDoc);
        assert(actualDoc,
               "Document " + tojson(resultDoc) +
                   " is not found in the result collection as expected ");
        assert.docEq(resultDoc, actualDoc);
    });

    assert(coll.drop());
}

const hintDoc1 = {
    _id: 1,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 1}
};
const hintDoc2 = {
    _id: 2,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 2}
};
const hintDoc3 = {
    _id: 3,
    [timeFieldName]: dateTime,
    [metaFieldName]: {"a": 3}
};

/************* Tests passing a hint to an update on a collection with a single index. *************/
// Query on and update the metaField using a forward collection scan: hint: {$natural 1}.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$lte: 2}},
        u: {$inc: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: {$natural: 1}
    }],
    resultDocList: [
        {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {"a": 11}},
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 12}},
        hintDoc3
    ],
    nModifiedBuckets: 2,
    expectedPlan: "COLLSCAN",
});

// Query on and update the metaField using a backward collection scan: hint: {$natural -1}.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$lte: 2}},
        u: {$inc: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: {$natural: -1}
    }],
    resultDocList: [
        {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {"a": 11}},
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 12}},
        hintDoc3
    ],
    nModifiedBuckets: 2,
    expectedPlan: "COLLSCAN",
});

// Query on and update the metaField using the metaField index as a hint, specifying the hint with
// an index specification document.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$lte: 2}},
        u: {$inc: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: {[metaFieldName]: 1}
    }],
    resultDocList: [
        {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {"a": 11}},
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 12}},
        hintDoc3
    ],
    nModifiedBuckets: 2,
    expectedPlan: "IXSCAN { meta: 1 }",
});

// Query on and update the metaField using a compound index on the metaField and timeField as a
// hint, specifying the hint with an index specification document.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1, [timeFieldName]: 1}],
    updateList: [{
        q: {[metaFieldName]: {a: 2}},
        u: {$inc: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: {[metaFieldName]: 1, [timeFieldName]: 1}
    }],
    resultDocList:
        [hintDoc1, {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 12}}, hintDoc3],
    nModifiedBuckets: 1,
    expectedPlan: "IXSCAN { meta: 1, control.min.time: 1, control.max.time: 1 }",
});

testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: -1, [timeFieldName]: 1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$gte: 2}},
        u: {$inc: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: {[metaFieldName]: -1, [timeFieldName]: 1}
    }],
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 12}},
        {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: {"a": 13}}
    ],
    nModifiedBuckets: 2,
    expectedPlan: "IXSCAN { meta: -1, control.min.time: 1, control.max.time: 1 }",
});

// Query on and update the metaField using a compound index on the metaField and timeField as a
// hint, specifying the hint with the index name string.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName]: 1, [timeFieldName]: 1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$gte: 2}},
        u: {$inc: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: metaFieldName + "_1_" + timeFieldName + "_1"
    }],
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 12}},
        {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: {"a": 13}}
    ],
    nModifiedBuckets: 2,
    expectedPlan: "IXSCAN { meta: 1, control.min.time: 1, control.max.time: 1 }",
});

// Query on and update the metaField using a compound index on the timeField and an embedded field
// of the metaField as hints, specifying the hint with the index name string.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: -1, [timeFieldName]: 1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$gte: 2}},
        u: {$set: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: metaFieldName + ".a_-1_" + timeFieldName + "_1"
    }],
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 10}},
        {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: {"a": 10}}
    ],
    nModifiedBuckets: 2,
    expectedPlan: "IXSCAN { meta.a: -1, control.min.time: 1, control.max.time: 1 }",
});

/************ Tests passing a hint to an update on a collection with multiple indexes. ************/
// Query on and update the metaField of a collection with multiple indexes without specifying a
// hint.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: 1}, {[timeFieldName]: -1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$gte: 2}},
        u: {$set: {[metaFieldName + ".a"]: 10}},
        multi: true,
    }],
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 10}},
        {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: {"a": 10}}
    ],
    nModifiedBuckets: 2,
    expectedPlan: "IXSCAN { meta.a: 1 }",
});

// Query on and update the metaField of a collection with multiple indexes using the timeField index
// as a hint. Note that this is the same update as above, but specifying the hint should cause a
// different index to be used.
testUpdateHintSucceeded({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: 1}, {[timeFieldName]: -1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$gte: 2}},
        u: {$set: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: {[timeFieldName]: -1}
    }],
    resultDocList: [
        hintDoc1,
        {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {"a": 10}},
        {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: {"a": 10}}
    ],
    nModifiedBuckets: 2,
    expectedPlan: "IXSCAN { control.max.time: -1, control.min.time: -1 }",
});

// A non-existent index specified with an index specification document should fail.
testUpdateHintFailed({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: 1}, {[timeFieldName]: -1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$gte: 2}},
        u: {$set: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: {"badIndexName": -1}
    }],
    resultDocList: [hintDoc1, hintDoc2, hintDoc3],
    nModifiedBuckets: 0,
    failCode: ErrorCodes.BadValue,
});

// A non-existent index specified with a string should fail.
testUpdateHintFailed({
    initialDocList: [hintDoc1, hintDoc2, hintDoc3],
    indexes: [{[metaFieldName + ".a"]: 1}, {[timeFieldName]: -1}],
    updateList: [{
        q: {[metaFieldName + ".a"]: {$gte: 2}},
        u: {$set: {[metaFieldName + ".a"]: 10}},
        multi: true,
        hint: "bad_index_name"
    }],
    resultDocList: [hintDoc1, hintDoc2, hintDoc3],
    nModifiedBuckets: 0,
    failCode: ErrorCodes.BadValue,
});
})();
