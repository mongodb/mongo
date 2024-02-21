/**
 * Tests the behavior of the 'postBatchResumeToken' and '$_resumeAfter' fields in 'find' and
 * 'getMore' requests and responses on the oplog.
 *
 * @tags: [
 * ]
 */

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({command: 2, query: 5})}}
});
rst.startSet();
rst.initiate();

const node = rst.getPrimary();

const dbName = "test";
const collName = jsTestName();

jsTestLog("Inserting some data");
// We will query the oplog for the entries corresponding to those inserts.  We insert one at time
// to avoid batching of vectored inserts.
const testData = [{_id: 0, ans: 42}, {_id: 1, ans: 42}, {_id: 2, ans: 42}];
testData.forEach(
    doc => assert.commandWorked(node.getDB(dbName).getCollection(collName).insert([doc])));

const localDb = node.getDB("local");
const kNullTS = new Timestamp(0, 0);

function assertExpectedResumeTokenFormat(res, isOplog = true) {
    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    const resumeToken = res.cursor.postBatchResumeToken;
    assert.eq(resumeToken.hasOwnProperty("ts"), isOplog, res);
    assert.eq(resumeToken.hasOwnProperty("$recordId"), !isOplog, res);
    return resumeToken;
}

// ---------------------------------------------------------------------------------------
jsTestLog("Running initial query on the oplog");
{
    const res = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42},
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true
    }));

    assert.eq(res.cursor.firstBatch.length, 1, res);
    assert.eq(res.cursor.firstBatch[0].o._id, 0, res);

    // Assert resume token is non-null.
    const resumeToken1 = assertExpectedResumeTokenFormat(res);
    assert.eq(timestampCmp(resumeToken1.ts, kNullTS), 1, res);

    // Kill the cursor before attempting to resume.
    assert.commandWorked(localDb.runCommand({killCursors: "oplog.rs", cursors: [res.cursor.id]}));

    jsTestLog("Resuming oplog collection scan fails");
    // This fails because '$_resumeAfter' expects a '$recordId' resume token, not a 'ts' resume
    // token. This is appropriate since resuming on the oplog should occur by timestamp, not by
    // record id.
    assert.commandFailedWithCode(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42},
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
        $_resumeAfter: resumeToken1
    }),
                                 ErrorCodes.BadValue);

    // Confirm that when we restart our oplog scan after the resume token, we see
    // the next expected document and the PBRT advances past the last observed token.
    const res2 = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42, ts: {"$gt": resumeToken1.ts}},
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
    }));

    assert.eq(res2.cursor.firstBatch.length, 1, res);
    assert.eq(res2.cursor.firstBatch[0].o._id, 1, res);

    const resumeToken2 = assertExpectedResumeTokenFormat(res2);
    assert.eq(timestampCmp(resumeToken2.ts, resumeToken1.ts), 1, res2);

    const res3 = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42, ts: {"$gt": resumeToken2.ts}},
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true,
    }));

    assert.eq(res3.cursor.firstBatch.length, 1, res);
    assert.eq(res3.cursor.firstBatch[0].o._id, 2, res);

    const resumeToken3 = assertExpectedResumeTokenFormat(res3);
    assert.eq(timestampCmp(resumeToken3.ts, resumeToken2.ts), 1, res3);
}
// ---------------------------------------------------------------------------------------
jsTestLog("Running initial tailable query on the oplog");
{
    const res = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42},
        hint: {$natural: 1},
        batchSize: 1,
        tailable: true,
        awaitData: true,
        $_requestResumeToken: true
    }));

    assert.eq(res.cursor.firstBatch.length, 1, res);
    assert.eq(res.cursor.firstBatch[0].o._id, 0, res);

    // Resume token should be non-null.
    const resumeToken1 = assertExpectedResumeTokenFormat(res);
    assert.eq(timestampCmp(resumeToken1.ts, kNullTS), 1, res);

    const cursorId = res.cursor.id;

    jsTest.log("Ensure that postBatchResumeToken attribute is returned for getMore command");
    const resGetMore1 =
        assert.commandWorked(localDb.runCommand({getMore: cursorId, collection: "oplog.rs"}));

    assert.eq(resGetMore1.cursor.nextBatch.length, 2, resGetMore1);
    assert.eq(resGetMore1.cursor.nextBatch[0].o._id, 1, resGetMore1);
    assert.eq(resGetMore1.cursor.nextBatch[1].o._id, 2, resGetMore1);

    // Resume token should be greater than the find command's.
    const resumeToken2 = assertExpectedResumeTokenFormat(resGetMore1);
    assert.eq(timestampCmp(resumeToken2.ts, resumeToken1.ts), 1, resGetMore1);

    jsTest.log(
        "Ensure that postBatchResumeToken attribute is returned for getMore command with no results");
    const resGetMore2 = assert.commandWorked(
        localDb.runCommand({getMore: cursorId, collection: "oplog.rs", maxTimeMS: 100}));

    // The results are exhausted, but the cursor stays alive.
    assert.eq(resGetMore2.cursor.nextBatch.length, 0, resGetMore2);
    assert.eq(resGetMore2.cursor.id, cursorId, resGetMore2);

    // Resume token should be the same as the first getMore.
    const resumeToken3 = assertExpectedResumeTokenFormat(resGetMore2);
    assert.eq(timestampCmp(resumeToken3.ts, resumeToken2.ts), 0);

    // Kill the tailable cursor.
    assert.commandWorked(localDb.runCommand({killCursors: "oplog.rs", cursors: [cursorId]}));
}
// ---------------------------------------------------------------------------------------
jsTest.log("Run find command on oplog with $_requestResumeToken disabled");
{
    const res = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42},
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: false
    }));

    assert(!res.cursor.hasOwnProperty("postBatchResumeToken"), res);
}
// ---------------------------------------------------------------------------------------
jsTest.log("Run find command on oplog with $_requestResumeToken defaulted to disabled");
{
    const res = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42},
        hint: {$natural: 1},
        batchSize: 1,
    }));

    assert(!res.cursor.hasOwnProperty("postBatchResumeToken"), res);
}
// ---------------------------------------------------------------------------------------
jsTest.log("Run find command on non-oplog with $_requestResumeToken requested");
{
    const res = assert.commandWorked(node.getDB(dbName).runCommand({
        find: collName,
        filter: {},
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true
    }));

    assert.eq(res.cursor.firstBatch.length, 1, res);
    assert.eq(res.cursor.firstBatch[0]._id, 0, res);

    assert.hasFields(res.cursor, ["postBatchResumeToken"]);
    assertExpectedResumeTokenFormat(res, false /* isOplog */);
}
// ---------------------------------------------------------------------------------------
jsTestLog("Running query on the oplog with no results");
{
    const res = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 43},
        hint: {$natural: 1},
        batchSize: 1,
        $_requestResumeToken: true
    }));

    assert.eq(res.cursor.firstBatch.length, 0, res);

    // Resume token should be non-null.
    const resumeToken = assertExpectedResumeTokenFormat(res);
    assert.eq(timestampCmp(resumeToken.ts, kNullTS), 1);
}
// ---------------------------------------------------------------------------------------
jsTestLog("Running tailable query on the oplog with no results");
{
    const res = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", ns: `${dbName}.${collName}`, "o.ans": 43},
        hint: {$natural: 1},
        batchSize: 1,
        tailable: true,
        awaitData: true,
        $_requestResumeToken: true
    }));

    assert.eq(res.cursor.firstBatch.length, 0, res);

    // Resume token should be non-null.
    const resumeToken1 = assertExpectedResumeTokenFormat(res);
    assert.eq(timestampCmp(resumeToken1.ts, kNullTS), 1);

    const cursorId = res.cursor.id;

    jsTest.log("Run a tailable getMore with no results");
    const resGetMore1 = assert.commandWorked(
        localDb.runCommand({getMore: cursorId, collection: "oplog.rs", maxTimeMS: 100}));

    assert.eq(resGetMore1.cursor.nextBatch.length, 0, resGetMore1);

    // Resume token should be equal to the find command's.
    const resumeToken2 = assertExpectedResumeTokenFormat(resGetMore1);
    assert.eq(timestampCmp(resumeToken2.ts, resumeToken1.ts), 0);

    // Insert dummy data so the next getMore should have a higher resume token.
    const latestOpTime = assert
                             .commandWorked(node.getDB(dbName).runCommand(
                                 {insert: collName + "_other", documents: [{dummy: 1}]}))
                             .opTime;

    jsTest.log("Run another tailable getMore with no results");
    const resGetMore2 = assert.commandWorked(
        localDb.runCommand({getMore: cursorId, collection: "oplog.rs", maxTimeMS: 100}));

    assert.eq(resGetMore2.cursor.nextBatch.length, 0, resGetMore2);

    // Resume token should be greater than the last getMore's.
    const resumeToken3 = assertExpectedResumeTokenFormat(resGetMore2);
    assert.eq(timestampCmp(resumeToken3.ts, resumeToken2.ts), 1, tojson({
                  currentResumeToken: resumeToken3,
                  lastResumeToken: resumeToken2,
                  latestOpTime: latestOpTime
              }));

    // Kill the tailable cursor.
    assert.commandWorked(localDb.runCommand({killCursors: "oplog.rs", cursors: [cursorId]}));
}

// ---------------------------------------------------------------------------------------
jsTestLog("Running query on the oplog with an empty batch");
{
    const res = assert.commandWorked(localDb.runCommand({
        find: "oplog.rs",
        filter: {op: "i", "o.ans": 42},
        hint: {$natural: 1},
        batchSize: 0,
        $_requestResumeToken: true
    }));

    assert.eq(res.cursor.firstBatch.length, 0, res);

    // Resume token should be null because the batch size is 0.
    const resumeToken1 = assertExpectedResumeTokenFormat(res);
    assert.eq(timestampCmp(resumeToken1.ts, kNullTS), 0);

    const cursorId = res.cursor.id;

    jsTest.log("Run a getMore that should return data");
    const resGetMore1 =
        assert.commandWorked(localDb.runCommand({getMore: cursorId, collection: "oplog.rs"}));

    assert.eq(resGetMore1.cursor.nextBatch.length, 3, resGetMore1);
    assert.eq(resGetMore1.cursor.nextBatch[0].o._id, 0, resGetMore1);
    assert.eq(resGetMore1.cursor.nextBatch[1].o._id, 1, resGetMore1);
    assert.eq(resGetMore1.cursor.nextBatch[2].o._id, 2, resGetMore1);
    assert.eq(resGetMore1.cursor.id, 0, resGetMore1);

    // Resume token should be greater than the find command's.
    const resumeToken2 = assertExpectedResumeTokenFormat(resGetMore1);
    assert.eq(timestampCmp(resumeToken2.ts, resumeToken1.ts), 1);
}

rst.stopSet();
