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
 *   tenant_migration_incompatible,
 *   cannot_run_during_upgrade_downgrade,
 *   # Does not support multiplanning, because it stashes documents beyond batch size.
 *   does_not_support_multiplanning_single_solutions,
 *   # The test explicitly tests with batchSize equal to 1 and the config fuzzer changes batchSize
 *   # that can cause a mimatch between results.
 *   does_not_support_config_fuzzer,
 * ]
 */

const collName = "resume_query_from_non_existent_record";
const coll = db[collName];

const kTestData = [
    {_id: 0, a: 1},
    {_id: 1, a: 2},
    {_id: 2, a: 3},
    {_id: 3, a: 4},
    {_id: 4, a: 5},
];
const numDocs = kTestData.length;

/**
 * Inserts the docs and then returns them in RID order.
 */
function insertDocsAndOrderByRecordId() {
    assert.commandWorked(coll.insert(kTestData));

    const docsByRecordId = coll.aggregate([
                                   {$addFields: {"rid_computed": {"$meta": "recordId"}}},
                                   {$sort: {rid_computed: 1}},
                                   {$project: {rid_computed: 0}},
                               ])
                               .toArray();

    return docsByRecordId;
}

const testCmd = function(cmdName, runQuery) {
    coll.drop();
    const docsInRidOrder = insertDocsAndOrderByRecordId();
    assert.eq(docsInRidOrder.length, numDocs);

    jsTestLog(`[${cmdName}] request a resumeToken then use it to resume.`);
    // Run the initial query and request to return a resume token. We're interested only in a single
    // document, so 'batchSize' is set to 1.
    let res = runQuery({$_requestResumeToken: true, batchSize: 1});
    assert.commandWorked(res);
    assert.eq(1, res.cursor.firstBatch.length);
    assert.docEq(docsInRidOrder[0], res.cursor.firstBatch[0]);

    // Make sure the query returned a resume token which will be used to resume the query from.
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    const resumeToken = res.cursor.postBatchResumeToken;

    // Kill the cursor before attempting to resume.
    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    {
        jsTestLog(
            `[${cmdName}] resumeAfter should successfully resume with a resume token pointing to a valid recordId.`,
        );
        res = runQuery({$_requestResumeToken: true, $_resumeAfter: resumeToken});
        assert.commandWorked(res);

        assert.eq(res.cursor.firstBatch.length, numDocs - 1);
        // We expect all but the first document to be returned.
        assert.docEq(docsInRidOrder.slice(1), res.cursor.firstBatch);
    }

    {
        jsTestLog(`[${cmdName}] startAt should successfully resume with a resume token pointing to a valid recordId.`);
        res = runQuery({$_requestResumeToken: true, $_startAt: resumeToken});
        assert.commandWorked(res);

        // We expect all but the first document to be returned.
        assert.docEq(docsInRidOrder.slice(1), res.cursor.firstBatch);
    }

    // Kill the cursor before attempting to resume.
    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    // Delete a document which corresponds to the saved resume token, so that we can guarantee it
    // does not exist.
    assert.commandWorked(coll.remove({_id: docsInRidOrder[0]._id}, {justOne: true}));

    {
        jsTestLog(
            `[${cmdName}] startAt should successfully resume with a resume token pointing to a deleted recordId.`,
        );
        // Try to resume the query using $_startAt from the same token and check that it tolerates
        // resumeToken pointing to a deleted recordId.
        res = runQuery({$_requestResumeToken: true, $_startAt: resumeToken});
        assert.commandWorked(res);
        assert.docEq(docsInRidOrder.slice(1), res.cursor.firstBatch);
    }

    {
        jsTestLog(
            `[${cmdName}] startAt with a resume token pointing to a deleted recordId should recover all the documents.`,
        );
        // Try to resume the query using $_startAt from the same token and check between the already
        // returned documents and this batch we recover all the documents. We set 'batchSize' to
        // numDocs+1 to ensure we get all the remaining documents.
        res =
            runQuery({$_requestResumeToken: true, batchSize: numDocs + 1, $_startAt: resumeToken});
        assert.commandWorked(res);
        assert.docEq(docsInRidOrder.slice(1), res.cursor.firstBatch);
    }

    {
        jsTestLog(`[${
            cmdName}] resumeAfter should fail with a resume token pointing to a deleted recordId.`);
        // Try to resume the query using $_resumeAfter from the same token and check that it fails
        // to position the cursor to the record specified in the resume token.
        res = runQuery({$_requestResumeToken: true, batchSize: 1, $_resumeAfter: resumeToken});
        assert.commandFailedWithCode(res, ErrorCodes.KeyNotFound);
    }

    const malformedResumeToken = {x: 1, $recordId: NumberLong(1), $initialSyncId: UUID()};

    {
        jsTestLog(`[${cmdName}] startAt should fail with a malformed resume token.`);
        res = assert.commandFailedWithCode(
            runQuery({$_requestResumeToken: true, batchSize: 1, $_startAt: malformedResumeToken}),
            ErrorCodes.BadValue,
        );
    }

    {
        jsTestLog(`[${cmdName}] resumeAfter should fail with a malformed resume token.`);
        res = assert.commandFailedWithCode(
            runQuery(
                {$_requestResumeToken: true, batchSize: 1, $_resumeAfter: malformedResumeToken}),
            ErrorCodes.BadValue,
        );
    }

    const invalidResumeToken = {$recordId: NumberLong(1), $initialSyncId: UUID()};

    {
        jsTestLog(
            `[${cmdName}] startAt should fail with an invalid initialSyncId in resume token.`);
        res = assert.commandFailedWithCode(
            runQuery({$_requestResumeToken: true, batchSize: 1, $_startAt: invalidResumeToken}),
            8132701,
        );
    }
    {
        jsTestLog(
            `[${cmdName}] resumeAfter should fail with an invalid initialSyncId in resume token.`);
        res = assert.commandFailedWithCode(
            runQuery({$_requestResumeToken: true, batchSize: 1, $_resumeAfter: invalidResumeToken}),
            8132701,
        );
    }
};

function testRandomDeletionAndResume(cmdName, runQuery) {
    coll.drop();
    const docsInRidOrder = insertDocsAndOrderByRecordId();
    assert.eq(docsInRidOrder.length, numDocs);

    jsTestLog(`[${cmdName}] Test random deletion and resume with $_startAt.`);

    // Randomly select a document and request a resume token for it
    const randomDocIndex = Math.floor(Math.random() * numDocs);
    const randomDoc = docsInRidOrder[randomDocIndex];

    jsTestLog(`[${cmdName}] Selected random document for resume token: ${tojson(randomDoc)}.`);

    // Run the initial query to get the resume token with a batch size enough to include the random
    // document.
    let res = runQuery({
        $_requestResumeToken: true,
        batchSize: randomDocIndex + 1,
    });
    assert.commandWorked(res);
    assert.contains(randomDoc, res.cursor.firstBatch);

    // Make sure the query returned a resume token which will be used to resume the query from.
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    const resumeToken = res.cursor.postBatchResumeToken;

    // Kill the cursor before attempting to resume.
    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [res.cursor.id]}));

    // Delete the randomly selected document.
    assert.commandWorked(coll.remove({_id: randomDoc._id}, {justOne: true}));
    jsTestLog(`[${cmdName}] Deleted random document: ${tojson(randomDoc)}.`);

    {
        jsTestLog(`[${cmdName}] $_startAt should resume correctly from a deleted record id`);

        res = runQuery({$_requestResumeToken: true, $_startAt: resumeToken});
        assert.commandWorked(res);

        // Filter out the deleted document from the expected results.
        const remainingDocs = docsInRidOrder.filter((doc) => doc._id !== randomDoc._id);
        assert.docEq(
            remainingDocs.filter((doc, index) => index >= randomDocIndex),
            res.cursor.firstBatch,
        );
    }
}

function runFindQuery(extraFields) {
    return db.runCommand(Object.assign({find: collName, hint: {$natural: 1}}, extraFields));
}

function runAggQuery(extraFields) {
    const batchSize = extraFields.batchSize;

    delete extraFields.batchSize;
    const obj = Object.assign({aggregate: collName, pipeline: [], hint: {$natural: 1}, cursor: {}},
                              extraFields);
    if (batchSize) {
        obj.cursor.batchSize = batchSize;
    }
    return db.runCommand(obj);
}

testCmd("Find", runFindQuery);
testCmd("Aggregate", runAggQuery);

testRandomDeletionAndResume("Find", runFindQuery);
testRandomDeletionAndResume("Aggregate", runAggQuery);
