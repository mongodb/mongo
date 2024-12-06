/**
 * Tests running the delete command on a time-series collection with concurrent modifications to the
 * collection. These deletes operate on the full bucket document by targeting them with their meta
 * field value.
 *
 * @tags: [
 *   # Fail points in this test do not exist on mongos.
 *   assumes_against_mongod_not_mongos,
 *   # $currentOp can't run with a readConcern other than 'local'.
 *   assumes_read_concern_unchanged,
 *   # This test only synchronizes deletes on the primary.
 *   assumes_read_preference_unchanged,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # Uses parallel shell to wait on fail point
 *   uses_parallel_shell,
 *   # Multi clients cannot share global fail points. When one client turns off a fail point, other
 *   # clients waiting on the fail point will get failed.
 *   multi_clients_incompatible,
 * ]
 */

import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const timeFieldName = "time";
const metaFieldName = "tag";
const collName = 't';
const dbName = jsTestName();

const testCases = {
    DROP_COLLECTION: 0,
    REPLACE_COLLECTION: 1,
    REPLACE_METAFIELD: 2
};

const validateDeleteIndex =
    (docsToInsert, deleteQuery, expectedErrorCode, testCase, newMetaField = null) => {
        const testDB = db.getSiblingDB(dbName);
        const awaitTestDelete = startParallelShell(funWithArgs(
            function(docsToInsert,
                     deleteQuery,
                     timeFieldName,
                     metaFieldName,
                     collName,
                     expectedErrorCode,
                     dbName) {
                const testDB = db.getSiblingDB(dbName);
                const coll = testDB.getCollection(collName);

                assert.commandWorked(testDB.createCollection(
                    coll.getName(),
                    {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

                assert.commandWorked(coll.insert(docsToInsert));

                assert.commandWorked(testDB.adminCommand(
                    {configureFailPoint: "hangDuringBatchRemove", mode: "alwaysOn"}));
                assert.commandFailedWithCode(
                    testDB.runCommand({delete: coll.getName(), deletes: deleteQuery}),
                    expectedErrorCode);

                coll.drop();
            },
            docsToInsert,
            deleteQuery,
            timeFieldName,
            metaFieldName,
            collName,
            expectedErrorCode,
            dbName));
        const coll = testDB.getCollection(collName);

        const childOp =
            waitForCurOpByFailPoint(testDB, coll.getFullName(), "hangDuringBatchRemove")[0];

        // Drop the collection in all test cases.
        assert(coll.drop());
        switch (testCase) {
            case testCases.REPLACE_COLLECTION:
                assert.commandWorked(testDB.createCollection(coll.getName()));
                break;
            case testCases.REPLACE_METAFIELD:
                assert.commandWorked(testDB.createCollection(
                    coll.getName(),
                    {timeseries: {timeField: timeFieldName, metaField: newMetaField}}));
                break;
        }

        assert.commandWorked(
            testDB.adminCommand({configureFailPoint: "hangDuringBatchRemove", mode: "off"}));

        // Wait for testDelete to finish.
        awaitTestDelete();
    };

const objA = {
    [timeFieldName]: ISODate(),
    "measurement": {"A": "cpu"},
    [metaFieldName]: {a: "A"}
};

// Attempt to delete from a collection that has been dropped.
validateDeleteIndex(
    [objA],
    [{q: {[metaFieldName]: {a: "A"}}, limit: 0}],
    [ErrorCodes.NamespaceNotFound, 8555700, 8555701],  // TODO (SERVER-85548): revisit error codes
    testCases.DROP_COLLECTION);

// Attempt to delete from a collection that has been replaced with a non-time-series collection.
validateDeleteIndex(
    [objA],
    [{q: {[metaFieldName]: {a: "A"}}, limit: 0}],
    [ErrorCodes.NamespaceNotFound, 8555700, 8555701],  // TODO (SERVER-85548): revisit error codes
    testCases.REPLACE_COLLECTION);
