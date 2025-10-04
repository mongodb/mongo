/**
 * Tests running the delete command with a hint on a time-series collection.
 * @tags: [
 *   # $currentOp can't run with a readConcern other than 'local'.
 *   assumes_read_concern_unchanged,
 *   # This test only synchronizes deletes on the primary.
 *   assumes_read_preference_unchanged,
 *   # Fail points in this test do not exist on mongos.
 *   assumes_against_mongod_not_mongos,
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
const collName = "t";
const dbName = jsTestName();
const collNamespace = dbName + "." + collName;

const validateDeleteIndex = (
    docsToInsert,
    expectedRemainingDocs,
    expectedNRemoved,
    deleteQuery,
    indexes,
    expectedPlan,
    {expectedErrorCode = null} = {},
) => {
    const testDB = db.getSiblingDB(dbName);
    const awaitTestDelete = startParallelShell(
        funWithArgs(
            function (
                docsToInsert,
                expectedRemainingDocs,
                expectedNRemoved,
                deleteQuery,
                indexes,
                timeFieldName,
                metaFieldName,
                collName,
                dbName,
                expectedErrorCode,
            ) {
                const testDB = db.getSiblingDB(dbName);
                const coll = testDB.getCollection(collName);

                assert.commandWorked(
                    testDB.createCollection(coll.getName(), {
                        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
                    }),
                );
                assert.commandWorked(coll.createIndexes(indexes));

                assert.commandWorked(coll.insert(docsToInsert));

                assert.commandWorked(
                    testDB.adminCommand({configureFailPoint: "hangBeforeChildRemoveOpFinishes", mode: "alwaysOn"}),
                );
                const res = expectedErrorCode
                    ? assert.commandFailedWithCode(
                          testDB.runCommand({delete: coll.getName(), deletes: deleteQuery}),
                          expectedErrorCode,
                      )
                    : assert.commandWorked(testDB.runCommand({delete: coll.getName(), deletes: deleteQuery}));
                assert.eq(res["n"], expectedNRemoved);
                assert.sameMembers(expectedRemainingDocs, coll.find({}, {_id: 0}).toArray());
                assert(coll.drop());
            },
            docsToInsert,
            expectedRemainingDocs,
            expectedNRemoved,
            deleteQuery,
            indexes,
            timeFieldName,
            metaFieldName,
            collName,
            dbName,
            expectedErrorCode,
        ),
    );

    for (let childCount = 0; childCount < deleteQuery.length; ++childCount) {
        const childCurOp = waitForCurOpByFailPoint(testDB, collNamespace, "hangBeforeChildRemoveOpFinishes")[0];

        // Assert the plan uses the expected index.
        if (!expectedErrorCode) {
            assert.eq(childCurOp["planSummary"], expectedPlan);
        }

        assert.commandWorked(
            testDB.adminCommand({configureFailPoint: "hangBeforeChildRemoveOpIsPopped", mode: "alwaysOn"}),
        );
        assert.commandWorked(testDB.adminCommand({configureFailPoint: "hangBeforeChildRemoveOpFinishes", mode: "off"}));

        waitForCurOpByFailPoint(testDB, collNamespace, "hangBeforeChildRemoveOpIsPopped");

        // Enable the hangBeforeChildRemoveOpFinishes fail point if this is not the last child.
        if (childCount + 1 < deleteQuery.length) {
            assert.commandWorked(
                testDB.adminCommand({configureFailPoint: "hangBeforeChildRemoveOpFinishes", mode: "alwaysOn"}),
            );
        }

        assert.commandWorked(testDB.adminCommand({configureFailPoint: "hangBeforeChildRemoveOpIsPopped", mode: "off"}));
    }

    // Wait for testDelete to finish.
    awaitTestDelete();
};

const objA = {
    [timeFieldName]: ISODate(),
    "measurement": {"A": "cpu"},
    [metaFieldName]: {a: "A"},
};
const objB = {
    [timeFieldName]: ISODate(),
    "measurement": {"A": "cpu"},
    [metaFieldName]: {b: "B"},
};
const objC = {
    [timeFieldName]: ISODate(),
    "measurement": {"A": "cpu"},
    [metaFieldName]: {c: "C"},
};

// Query using a $natural hint.
validateDeleteIndex(
    [objA, objB, objC],
    [objB, objC],
    1,
    [{q: {[metaFieldName]: {a: "A"}}, limit: 0, hint: {$natural: 1}}],
    [{[metaFieldName]: 1}],
    "COLLSCAN",
);
validateDeleteIndex(
    [objA, objB, objC],
    [objB, objC],
    1,
    [{q: {[metaFieldName]: {a: "A"}}, limit: 0, hint: {$natural: -1}}],
    [{[metaFieldName]: 1}],
    "COLLSCAN",
);

// Query using the metaField index as a hint.
validateDeleteIndex(
    [objA, objB, objC],
    [objB, objC],
    1,
    [{q: {[metaFieldName]: {a: "A"}}, limit: 0, hint: {[metaFieldName]: 1}}],
    [{[metaFieldName]: 1}],
    "IXSCAN { meta: 1 }",
);

// Query on a collection with a compound index using the timeField and metaField indexes as
// hints.
validateDeleteIndex(
    [objA, objB, objC],
    [objB, objC],
    1,
    [{q: {[metaFieldName]: {a: "A"}}, limit: 0, hint: {[timeFieldName]: 1, [metaFieldName]: 1}}],
    [{[timeFieldName]: 1, [metaFieldName]: 1}],
    "IXSCAN { control.min.time: 1, control.max.time: 1, meta: 1 }",
);

// Query on a collection with a compound index using the timeField and metaField indexes as
// hints
validateDeleteIndex(
    [objA, objB, objC],
    [objA, objC],
    1,
    [{q: {[metaFieldName]: {b: "B"}}, limit: 0, hint: {[timeFieldName]: -1, [metaFieldName]: 1}}],
    [{[timeFieldName]: -1, [metaFieldName]: 1}],
    "IXSCAN { control.max.time: -1, control.min.time: -1, meta: 1 }",
);

// Query on a collection with a compound index using the timeField and metaField index names
// as hints.
validateDeleteIndex(
    [objA, objB, objC],
    [objA, objC],
    1,
    [
        {
            q: {[metaFieldName]: {b: "B"}},
            limit: 0,
            hint: timeFieldName + "_1_" + metaFieldName + "_1",
        },
    ],
    [{[timeFieldName]: 1, [metaFieldName]: 1}],
    "IXSCAN { control.min.time: 1, control.max.time: 1, meta: 1 }",
);

// Query on a collection with multiple indexes using the timeField index as a hint.
validateDeleteIndex(
    [objA, objB, objC],
    [objA, objB],
    1,
    [{q: {[metaFieldName]: {c: "C"}}, limit: 0, hint: {[timeFieldName]: 1}}],
    [{[metaFieldName]: -1}, {[timeFieldName]: 1}],
    "IXSCAN { control.min.time: 1, control.max.time: 1 }",
);

// Query on a collection with multiple indexes using an invalid index name.
validateDeleteIndex(
    [objA, objB, objC],
    [objA, objB, objC],
    0,
    [{q: {[metaFieldName]: {c: "C"}}, limit: 0, hint: "test_hint"}],
    [{[metaFieldName]: -1}, {[timeFieldName]: 1}],
    "IXSCAN { control.min.time: 1, control.max.time: 1 }",
    {expectedErrorCode: ErrorCodes.BadValue},
);

// Query on a collection with multiple indexes using an invalid index spec.
validateDeleteIndex(
    [objA, objB, objC],
    [objA, objB, objC],
    0,
    [{q: {[metaFieldName]: {c: "C"}}, limit: 0, hint: {"test_hint": 1}}],
    [{[metaFieldName]: -1}, {[timeFieldName]: 1}],
    "IXSCAN { control.min.time: 1, control.max.time: 1 }",
    {expectedErrorCode: ErrorCodes.BadValue},
);
