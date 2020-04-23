/**
 * Tests that using the $_resumeAfter option in a query against the oplog does not invariant.
 */

(function() {
"use strict";

const testName = "resume_after_against_oplog";
const rst = new ReplSetTest({nodes: 1, name: testName});
rst.startSet();
rst.initiate();

const node = rst.getPrimary();

const dbName = "test";
const collName = testName;

jsTestLog("Inserting some data");
// We will query the oplog for the entries corresponding to those inserts.
const testData = [{_id: 0, ans: 42}, {_id: 1, ans: 42}];
assert.commandWorked(node.getDB(dbName).getCollection(collName).insert(testData));

const localDb = node.getDB("local");

jsTestLog("Running initial query on the oplog");
const firstRes = assert.commandWorked(localDb.runCommand({
    find: "oplog.rs",
    filter: {op: "i", "o.ans": 42},
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true
}));

const firstDoc = firstRes.cursor.firstBatch[0];
assert.eq(firstDoc.o._id, 0);
assert.hasFields(firstRes.cursor, ["postBatchResumeToken"]);
const resumeToken = firstRes.cursor.postBatchResumeToken;

// Kill the cursor before attempting to resume.
assert.commandWorked(localDb.runCommand({killCursors: "oplog.rs", cursors: [firstRes.cursor.id]}));

jsTestLog("Resuming oplog collection scan from the last recordId we saw");
const secondRes = assert.commandWorked(localDb.runCommand({
    find: "oplog.rs",
    filter: {op: "i", "o.ans": 42},
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: resumeToken
}));

const secondDoc = secondRes.cursor.firstBatch[0];
assert.eq(secondDoc.o._id, 1);

// Make sure the second result differs from the first.
assert.neq(firstDoc, secondDoc);

rst.stopSet();
})();
