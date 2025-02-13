/**
 * Tests behavior when resuming queries with invalid or missing records:
 * - Tests that attempting to resume with '$_resumeAfter' targeting a non-existent record
 *   raises an error.
 * - Tests that resuming with '$_startAt' skips non-existent records and resumes from
 *   the nearest valid record.
 *
 * Suites which require retryable writes may involve a change in the primary node during the course
 * of the test. However, $_requestResumeToken and a subsequent $_resumeAfter/ $_startAt must be
 * directed at the same node, since they rely on a record id which is assigned internally by a given
 * node.
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
 *   # The test explicitly tests with batchSize equal to 1 and the config fuzzer changes batchSize
 *   # that can cause a mimatch between results.
 *   does_not_support_config_fuzzer,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const collName = "resume_query_from_non_existent_record";
const coll = db[collName];

/**
 * Inserts 'testData' into 'coll' and retrieves the recordIds of the inserted documents. These
 * recordIds are used to verify correct positioning when resuming a collection scan.
 */
function insertDocsAndGetRecordIds(coll, testData) {
    assert.commandWorked(coll.insert(testData));
    const recordIdMap = {};
    const res = assert.commandWorked(
        db.runCommand({find: coll.getName(), hint: {$natural: 1}, showRecordId: true}));
    res.cursor.firstBatch.forEach(doc => {
        recordIdMap[doc._id] = doc.$recordId;
    });
    return recordIdMap;
}

function verifyBatchContents(res, testData, savedData, recordIdMap) {
    const firstBatch = res.cursor.firstBatch;
    assert.eq(1, firstBatch.length);
    const doc = firstBatch[0];
    assert.eq(recordIdMap[doc._id],
              res.cursor.postBatchResumeToken.$recordId,
              `Resume token mismatch for _id: ${doc._id}`);
    assert.contains(doc, testData);
    assert.neq(savedData[0], doc);
}

const testFindCmd = function() {
    coll.drop();

    const testData = [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}];
    const recordIdMap = insertDocsAndGetRecordIds(coll, testData);

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

    jsTestLog(
        "[Find] resumeAfter should successfully resume with a resume token pointing to a valid recordId.");
    res = assert.commandWorked(db.runCommand({
        find: collName,
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }));
    verifyBatchContents(res, testData, savedData, recordIdMap);

    jsTestLog(
        "[Find] startAt should successfully resume with a resume token pointing to a valid recordId.");
    res = assert.commandWorked(db.runCommand({
        find: collName,
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
        $_startAt: resumeToken
    }));
    verifyBatchContents(res, testData, savedData, recordIdMap);

    // Kill the cursor before attempting to resume.
    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    // Delete a document which corresponds to the saved resume token, so that we can guarantee it
    // does not exist.
    assert.commandWorked(coll.remove({_id: savedData[0]._id}, {justOne: true}));

    jsTestLog(
        "[Find] startAt should successfully resume with a resume token pointing to a deleted recordId.");
    // Try to resume the query using $_startAt from the same token and check that it tolerates
    // resumeToken pointing to a deleted recordId.
    res = assert.commandWorked(db.runCommand({
        find: collName,
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
        $_startAt: resumeToken
    }));
    verifyBatchContents(res, testData, savedData, recordIdMap);

    jsTestLog("[Find] resumeAfter should fail with a resume token pointing to a deleted recordId.");
    // Try to resume the query using $_resumeAfter from the same token and check that it fails to
    // position the cursor to the record specified in the resume token.
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
    const recordIdMap = insertDocsAndGetRecordIds(coll, testData);

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
    verifyBatchContents(res, testData, savedData, recordIdMap);

    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));
    assert.commandWorked(coll.remove({_id: savedData[0]._id}, {justOne: true}));

    jsTestLog(
        "[Aggregate] startAt should successfully resume with a resume token pointing to a deleted recordId.");
    // Try to resume the query using $_startAt from the same token and check that it tolerates
    // resumeToken pointing to a deleted recordId.
    res = assert.commandWorked(db.runCommand({
        aggregate: collName,
        pipeline: [],
        hint: {$natural: 1},
        cursor: {batchSize: 1},
        $_requestResumeToken: true,
        $_startAt: resumeToken
    }));
    verifyBatchContents(res, testData, savedData, recordIdMap);

    jsTestLog(
        "[Aggregate] resumeAfter should fail with a resume token pointing to a deleted recordId.");

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
