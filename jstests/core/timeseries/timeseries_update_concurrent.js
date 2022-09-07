/**
 * Tests running the update command on a time-series collection with concurrent modifications to the
 * collection.
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
load("jstests/libs/parallel_shell_helpers.js");

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

const timeFieldName = "time";
const metaFieldName = "tag";
const collName = 't';
const dbName = jsTestName();

const testCases = {
    DROP_COLLECTION: 0,
    REPLACE_COLLECTION: 1,
    REPLACE_METAFIELD: 2
};

const validateUpdateIndex = (initialDocList,
                             updateList,
                             testType,
                             failCode,
                             newMetaField = null) => {
    const testDB = db.getSiblingDB(dbName);
    const awaitTestUpdate = startParallelShell(funWithArgs(
        function(
            dbName, collName, timeFieldName, metaFieldName, initialDocList, updateList, failCode) {
            const testDB = db.getSiblingDB(dbName);
            const coll = testDB.getCollection(collName);

            assert.commandWorked(testDB.createCollection(
                coll.getName(),
                {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

            assert.commandWorked(coll.insert(initialDocList));

            assert.commandWorked(testDB.adminCommand(
                {configureFailPoint: "hangDuringBatchUpdate", mode: "alwaysOn"}));

            assert.commandFailedWithCode(
                testDB.runCommand({update: coll.getName(), updates: updateList}), failCode);

            coll.drop();
        },
        dbName,
        collName,
        timeFieldName,
        metaFieldName,
        initialDocList,
        updateList,
        failCode));

    const coll = testDB.getCollection(collName);
    const childOp = waitForCurOpByFailPoint(testDB, coll.getFullName(), "hangDuringBatchUpdate")[0];

    // Drop the collection in all test cases.
    assert(coll.drop());
    switch (testType) {
        case testCases.REPLACE_COLLECTION:
            assert.commandWorked(testDB.createCollection(coll.getName()));
            break;
        case testCases.REPLACE_METAFIELD:
            assert.commandWorked(testDB.createCollection(
                coll.getName(), {timeseries: {timeField: timeFieldName, metaField: newMetaField}}));
            break;
    }

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "hangDuringBatchUpdate", mode: "off"}));

    // Wait for testUpdate to finish.
    awaitTestUpdate();
};

const docs = [
    {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: {a: "A"}, "measurement": {"m": 1}},
    {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: {a: "A"}, "measurement": {"n": 3}},
    {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: {a: "B"}},
];

// Attempt to update a document in a collection that has been dropped.
validateUpdateIndex(
    docs,
    [{q: {[metaFieldName]: {a: "B"}}, u: {$set: {[metaFieldName]: {c: "C"}}}, multi: true}],
    testCases.DROP_COLLECTION,
    ErrorCodes.NamespaceNotFound);

// Attempt to update a document in a collection that has been replaced with a non-time-series
// collection.
validateUpdateIndex(
    docs,
    [{q: {[metaFieldName]: {a: "B"}}, u: {$set: {[metaFieldName]: {c: "C"}}}, multi: true}],
    testCases.REPLACE_COLLECTION,
    ErrorCodes.NamespaceNotFound);

// Attempt to update a document in a collection that has been replaced with a new time-series
// collection with a different metaField.
validateUpdateIndex(
    docs,
    [{q: {[metaFieldName]: {a: "B"}}, u: {$set: {[metaFieldName]: {c: "C"}}}, multi: true}],
    testCases.REPLACE_METAFIELD,
    ErrorCodes.InvalidOptions,
    "meta");
})();
