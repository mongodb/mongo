/**
 * Tests that passing API parameters into 'getMore' commands should fail.
 * @tags: [requires_getmore, requires_fcv_46]
 */

(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");
testColl.drop();

const nDocs = 20;
const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());

const cursorId =
    assert.commandWorked(testDB.runCommand({find: testColl.getName(), batchSize: 1})).cursor.id;

// Verify that passing in any API parameters to the 'getMore' command should fail.
assert.commandFailedWithCode(
    testDB.runCommand({getMore: cursorId, collection: testColl.getName(), apiVersion: "1"}),
    4937600);
assert.commandFailedWithCode(
    testDB.runCommand({getMore: cursorId, collection: testColl.getName(), apiStrict: false}),
    4937600);
assert.commandFailedWithCode(
    testDB.runCommand(
        {getMore: cursorId, collection: testColl.getName(), apiDeprecationErrors: false}),
    4937600);

// Verify that a 'getMore' without any API parameters will still succeed.
assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: testColl.getName()}));
})();
