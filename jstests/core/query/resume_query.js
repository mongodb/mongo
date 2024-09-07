/**
 * Test that we can use the $_resumeAfter and $_requestResumeToken options to resume a query.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: killCursors.
 *   not_allowed_with_signed_security_token,
 *   assumes_against_mongod_not_mongos,
 *   multiversion_incompatible,
 *   requires_getmore,
 *   requires_persistence,
 *   no_selinux,
 *   cannot_run_during_upgrade_downgrade,
 *   # Multiplanning breaks postBatchResumeTokens, as they don't account for PlanExecutor's stash
 *   does_not_support_multiplanning_single_solutions,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const testName = TestData.testName;

const testFindCmd = function() {
    const testDb = db.getSiblingDB(testName);
    assert.commandWorked(testDb.dropDatabase());

    jsTestLog("[Find] Setting up the data.");
    const testData = [{_id: 0, a: 1}, {_id: 1, b: 2}, {_id: 2, c: 3}, {_id: 3, d: 4}];
    assert.commandWorked(testDb.test.insert(testData));
    jsTestLog("[Find] Running the initial query.");
    let res = assert.commandWorked(testDb.runCommand(
        {find: "test", hint: {$natural: 1}, batchSize: 1, $_requestResumeToken: true}));
    assert.eq(1, res.cursor.firstBatch.length);
    assert.contains(res.cursor.firstBatch[0], testData);
    let queryData = res.cursor.firstBatch;
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    let resumeToken = res.cursor.postBatchResumeToken;

    // Kill the cursor before attempting to resume.
    assert.commandWorked(testDb.runCommand({killCursors: "test", cursors: [res.cursor.id]}));

    jsTestLog("[Find] Running the second query after killing the cursor.");
    res = assert.commandWorked(testDb.runCommand({
        find: "test",
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }));
    assert.eq(1, res.cursor.firstBatch.length);
    // The return value should not be the same as the one before.
    assert.neq(queryData[0], res.cursor.firstBatch[0]);
    assert.contains(res.cursor.firstBatch[0], testData);
    queryData.push(res.cursor.firstBatch[0]);
    let cursorId = res.cursor.id;

    jsTestLog("[Find] Running getMore.");
    res = assert.commandWorked(
        testDb.runCommand({getMore: cursorId, collection: "test", batchSize: 1}));
    queryData.push(res.cursor.nextBatch[0]);
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    resumeToken = res.cursor.postBatchResumeToken;

    // Kill the cursor before attempting to resume.
    assert.commandWorked(testDb.runCommand({killCursors: "test", cursors: [res.cursor.id]}));

    jsTestLog("[Find] Testing resume from getMore");
    res = assert.commandWorked(testDb.runCommand({
        find: "test",
        hint: {$natural: 1},
        batchSize: 10,
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }));
    assert.eq(1, res.cursor.firstBatch.length);
    // This should have exhausted the collection.
    assert.eq(0, res.cursor.id);
    queryData.push(res.cursor.firstBatch[0]);

    assert.sameMembers(testData, queryData);
};

const testAggregateCmd = function(addDummyStage) {
    if (!FeatureFlagUtil.isEnabled(db, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled.");
        return;
    }

    // Optimizations in the aggregation code mean there are significant differences between
    // an "empty" aggregate and one with stages that aren't merged into the initial find, so
    // we use a redact stage that we know (but the server doesn't) won't redact anything to
    // disable those optimizations.
    const logPrefix =
        addDummyStage ? "[Aggregate non-empty pipeline] " : "[Aggregate empty pipeline] ";
    const pipeline = addDummyStage ? [{
        $redact: {$cond: {if: {$eq: ["$d", "never happens"]}, then: "$$PRUNE", else: "$$KEEP"}}
    }]
                                   : [];
    const testDb = db.getSiblingDB(testName);
    assert.commandWorked(testDb.dropDatabase());

    jsTestLog(logPrefix + "Setting up the data.");
    const testData = [{_id: 0, a: 1}, {_id: 1, b: 2}, {_id: 2, c: 3}, {_id: 3, d: 4}];
    assert.commandWorked(testDb.test.insert(testData));
    jsTestLog(logPrefix + "Running the initial query.");
    let res = assert.commandWorked(testDb.runCommand({
        aggregate: "test",
        pipeline: pipeline,
        hint: {$natural: 1},
        cursor: {batchSize: 1},
        $_requestResumeToken: true
    }));
    assert.eq(1, res.cursor.firstBatch.length);
    assert.contains(res.cursor.firstBatch[0], testData);
    let queryData = res.cursor.firstBatch;
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    let resumeToken = res.cursor.postBatchResumeToken;

    assert.commandWorked(testDb.runCommand({killCursors: "test", cursors: [res.cursor.id]}));

    jsTestLog(logPrefix + "Running the second query after killing the cursor.");
    res = assert.commandWorked(testDb.runCommand({
        aggregate: "test",
        pipeline: pipeline,
        hint: {$natural: 1},
        cursor: {batchSize: 1},
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }));
    assert.eq(1, res.cursor.firstBatch.length);
    assert.neq(queryData[0], res.cursor.firstBatch[0]);
    assert.contains(res.cursor.firstBatch[0], testData);
    queryData.push(res.cursor.firstBatch[0]);
    let cursorId = res.cursor.id;

    jsTestLog(logPrefix + "Running getMore.");
    res = assert.commandWorked(
        testDb.runCommand({getMore: cursorId, collection: "test", batchSize: 1}));
    queryData.push(res.cursor.nextBatch[0]);
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    resumeToken = res.cursor.postBatchResumeToken;

    assert.commandWorked(testDb.runCommand({killCursors: "test", cursors: [res.cursor.id]}));

    jsTestLog(logPrefix + "Testing resume from getMore");
    res = assert.commandWorked(testDb.runCommand({
        aggregate: "test",
        pipeline: pipeline,
        hint: {$natural: 1},
        cursor: {batchSize: 10},
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken
    }));
    assert.eq(1, res.cursor.firstBatch.length);
    assert.eq(0, res.cursor.id);
    queryData.push(res.cursor.firstBatch[0]);

    assert.sameMembers(testData, queryData);
};

testFindCmd();
testAggregateCmd(true);
testAggregateCmd(false);
