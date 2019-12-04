/**
 * Test that we can use the $_resumeAfter and $_requestResumeToken options to resume a query
 * even after the node has been restarted.
 *
 * @tags: [requires_find_command,requires_persistence]
 */

(function() {
"use strict";
const testName = TestData.testName;
let conn = MongoRunner.runMongod();
let db = conn.getDB(testName);

jsTestLog("Setting up the data.");
const testData = [{_id: 0, a: 1}, {_id: 1, b: 2}, {_id: 2, c: 3}, {_id: 3, d: 4}];
assert.commandWorked(db.test.insert(testData));

jsTestLog("Running the initial query.");
let res = assert.commandWorked(
    db.runCommand({find: "test", hint: {$natural: 1}, batchSize: 2, $_requestResumeToken: true}));
assert.eq(2, res.cursor.firstBatch.length);
assert.contains(res.cursor.firstBatch[0], testData);
let queryData = res.cursor.firstBatch;
assert.hasFields(res.cursor, ["postBatchResumeToken"]);
let resumeToken = res.cursor.postBatchResumeToken;

jsTestLog("Restarting the node.");
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({restart: conn, cleanData: false});
db = conn.getDB(testName);

jsTestLog("Running the second query after restarting the node.");
res = assert.commandWorked(db.runCommand({
    find: "test",
    hint: {$natural: 1},
    batchSize: 10,
    $_requestResumeToken: true,
    $_resumeAfter: resumeToken
}));
// This should have exhausted the collection.
assert.eq(0, res.cursor.id);
assert.eq(2, res.cursor.firstBatch.length);
queryData = queryData.concat(res.cursor.firstBatch);
assert.sameMembers(testData, queryData);
MongoRunner.stopMongod(conn);
})();
