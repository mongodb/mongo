/**
 * Tests that an abortIndexBuild change event is emitted when an index build aborts.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 * ]
 */

import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.coll;

coll.drop();
assert.commandWorked(coll.insert([{_id: 0, a: 0, b: 0}, {_id: 1, a: 1, b: 0}]));

const pipeline = [{$changeStream: {showExpandedEvents: true, showSystemEvents: true}}];
const cst = new ChangeStreamTest(testDB);
const cursor = cst.startWatchingChanges({pipeline, collection: coll});

const indexes = [
    {v: 2, key: {a: 1}, name: "a_1", unique: true},
    {v: 2, key: {b: 1}, name: "b_1", unique: true},
];

// Try to create unique indexes on "a" and "b", which should fail since the inserted documents
// contain duplicate values for "b".
assert.commandFailedWithCode(coll.runCommand({createIndexes: coll.getName(), indexes: indexes}),
                             ErrorCodes.DuplicateKey);

const assertNextIndexBuildEvent = function(operationType) {
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: {
            operationType: operationType,
            ns: {
                db: testDB.getName(),
                coll: coll.getName(),
            },
            operationDescription: {indexes: indexes},
        },
    });
};
assertNextIndexBuildEvent("startIndexBuild");
assertNextIndexBuildEvent("abortIndexBuild");
