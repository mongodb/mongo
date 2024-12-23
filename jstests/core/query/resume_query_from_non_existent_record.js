/**
 * Test that an error is raised when we try to resume a query from a record which doesn't exist.
 *
 * Suites which require retryable writes may involve a change in the primary node during the course
 * of the test. However, $_requestResumeToken and a subsequent $_resumeAfter must be directed at the
 * same node, since they rely on a record id which is assigned internally by a given node.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: killCursors.
 *   not_allowed_with_signed_security_token,
 *   assumes_against_mongod_not_mongos,
 *   multiversion_incompatible,
 *   requires_non_retryable_writes,
 *   cannot_run_during_upgrade_downgrade,
 *   # Does not support multiplanning, because it stashes documents beyond batch size.
 *   does_not_support_multiplanning_single_solutions,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const collName = "resume_query_from_non_existent_record";
const coll = db[collName];

const testFindCmd = function() {
    coll.drop();

    const testData = [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}];
    assert.commandWorked(coll.insert(testData));

    jsTestLog("[Find] request a resumeToken then use it to resume.");
    // Run the initial query and request to return a resume token. We're interested only in a single
    // document, so 'batchSize' is set to 1.
    let res = assert.commandWorked(db.runCommand(
        {find: collName, hint: {$natural: 1}, batchSize: 1, $_requestResumeToken: true}));
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

    jsTestLog(
        "[Find] Delete the document which corresponds to the saved resume token, then resumeAfter should fail.");
    // Delete a document which corresponds to the saved resume token, so that we can guarantee it
    // does not exist.
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
};

const testAggregateCmd = function() {
    if (!FeatureFlagUtil.isEnabled(db, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled.");
        return;
    }
    coll.drop();

    const testData = [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}];
    assert.commandWorked(coll.insert(testData));

    jsTestLog("[Aggregate] request a resumeToken then use it to resume.");
    let res = assert.commandWorked(db.runCommand({
        aggregate: collName,
        pipeline: [],
        hint: {$natural: 1},
        cursor: {batchSize: 1},
        $_requestResumeToken: true
    }));
    assert.eq(1, res.cursor.firstBatch.length);
    assert.contains(res.cursor.firstBatch[0], testData);
    const savedData = res.cursor.firstBatch;

    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    const resumeToken = res.cursor.postBatchResumeToken;

    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    res = assert.commandWorked(db.runCommand({
        aggregate: collName,
        pipeline: [],
        hint: {$natural: 1},
        cursor: {batchSize: 1},
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }));
    assert.eq(1, res.cursor.firstBatch.length);
    assert.contains(res.cursor.firstBatch[0], testData);
    assert.neq(savedData[0], res.cursor.firstBatch[0]);

    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    jsTestLog(
        "[Aggregate] Delete the document which corresponds to the saved resume token, then resumeAfter should fail.");
    assert.commandWorked(coll.remove({_id: savedData[0]._id}, {justOne: true}));

    assert.commandFailedWithCode(db.runCommand({
        aggregate: collName,
        pipeline: [],
        hint: {$natural: 1},
        cursor: {batchSize: 1},
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }),
                                 ErrorCodes.KeyNotFound);
};

testFindCmd();
testAggregateCmd();
