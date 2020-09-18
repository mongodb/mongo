/**
 * Test that an error is raised when we try to resume a query from a record which doesn't exist.
 *
 * Suites which require retryable writes may involve a change in the primary node during the course
 * of the test. However, $_requestResumeToken and a subsequent $_resumeAfter must be directed at the
 * same node, since they rely on a record id which is assigned internally by a given node.
 *
 * @tags: [
 *  assumes_against_mongod_not_mongos,
 *  requires_find_command,
 *  requires_non_retryable_writes,
 *  multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

const collName = "resume_query_from_non_existent_record";
const coll = db[collName];

coll.drop();

const testData = [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}];
assert.commandWorked(coll.insert(testData));

// Run the initial query and request to return a resume token. We're interested only in a single
// document, so 'batchSize' is set to 1.
let res = assert.commandWorked(
    db.runCommand({find: collName, hint: {$natural: 1}, batchSize: 1, $_requestResumeToken: true}));
assert.eq(1, res.cursor.firstBatch.length);
assert.contains(res.cursor.firstBatch[0], testData);
const savedData = res.cursor.firstBatch;

// Make sure the query returned a resume token which will be used to resume the query from.
assert.hasFields(res.cursor, ["postBatchResumeToken"]);
const resumeToken = res.cursor.postBatchResumeToken;

// Kill the cursor before attempting to resume.
assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

// Try to resume the query from the saved resume token.
res = assert.commandWorked(db.runCommand({
    find: collName,
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: resumeToken
}));
assert.eq(1, res.cursor.firstBatch.length);
assert.contains(res.cursor.firstBatch[0], testData);
assert.neq(savedData[0], res.cursor.firstBatch[0]);

// Kill the cursor before attempting to resume.
assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

// Delete a document which corresponds to the saved resume token, so that we can guarantee it does
// not exist.
assert.commandWorked(coll.remove({_id: savedData[0]._id}, {justOne: true}));

// Try to resume the query from the same token and check that it fails to position the cursor to
// the record specified in the resume token.
assert.commandFailedWithCode(db.runCommand({
    find: collName,
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: resumeToken
}),
                             ErrorCodes.KeyNotFound);
})();
