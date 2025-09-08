/**
 * Test that find and aggregate queries that resume with '$_startAt' always execute in the classic
 * engine.
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
 *   # Secondary read preference passthroughs don't set it on the explain which causes an
 *   # $initialSyncId mismatch.
 *   assumes_read_preference_unchanged,
 * ]
 */

import {getEngine} from "jstests/libs/analyze_plan.js";

const collName = "start_at_blocked_in_sbe";
const coll = db[collName];

const testStartAtNotSbe = function() {
    coll.drop();

    const testData = [
        {_id: 0, a: 1},
        {_id: 1, a: 2},
        {_id: 2, a: 3},
    ];
    assert.commandWorked(coll.insert(testData));

    jsTestLog("Request a resumeToken then use it to resume.");
    // Run the initial query and request to return a resume token. We're interested only in a single
    // document, so 'batchSize' is set to 1.
    let res = assert.commandWorked(
        db.runCommand(
            {find: collName, hint: {$natural: 1}, batchSize: 1, $_requestResumeToken: true}),
    );
    assert.eq(1, res.cursor.firstBatch.length);
    assert.contains(res.cursor.firstBatch[0], testData);
    const savedData = res.cursor.firstBatch;

    // Make sure the query returned a resume token which will be used to resume the query from.
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    const resumeToken = res.cursor.postBatchResumeToken;

    // Kill the cursor before attempting to resume.
    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    // Delete a document which corresponds to the saved resume token, so that we can guarantee it
    // does not exist.
    assert.commandWorked(coll.remove({_id: savedData[0]._id}, {justOne: true}));

    jsTestLog("[Find] resumeAfter with $_startAt should never execute in SBE.");
    let explain = assert.commandWorked(
        db.runCommand({
            explain: {
                find: collName,
                hint: {$natural: 1},
                batchSize: 1,
                $_requestResumeToken: true,
                $_startAt: resumeToken,
            },
        }),
    );
    assert.eq(getEngine(explain), "classic", tojson(explain));

    jsTestLog("[Aggregate] resumeAfter with $_startAt should never execute in SBE.");
    explain = assert.commandWorked(
        db.runCommand({
            explain: {
                aggregate: collName,
                pipeline: [],
                hint: {$natural: 1},
                cursor: {batchSize: 1},
                $_requestResumeToken: true,
                $_startAt: resumeToken,
            },
        }),
    );
    assert.eq(getEngine(explain), "classic", tojson(explain));
};

testStartAtNotSbe();
