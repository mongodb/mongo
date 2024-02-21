/**
 * Test that optimized collection scan on the oplog collection can be applied and executed
 * successfully.
 * @tags: [
 *   requires_replication,
 * ]
 */
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("oplog_scan_optimizations");
const localDb = rst.getPrimary().getDB("local");
const collName = "oplog_scan_optimizations";
const oplogCollName = "oplog.rs";
const coll = db[collName];
const oplogColl = localDb[oplogCollName];

coll.drop();

// Insert several documents individually so that we can use a cursor to fetch them in multiple
// batches.
const testData = [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}];
testData.forEach(doc => {
    assert.commandWorked(coll.insert([doc]));
});

// Run the initial query and request to return a resume token. We will also request to return a
// recordId for the document as it will
// be used as a $_resumeAfter token later.
let res = assert.commandWorked(localDb.runCommand({
    find: oplogCollName,
    filter: {op: "i", "o.a": {$gte: 1}},
    hint: {$natural: 1},
    batchSize: 1,
    showRecordId: true,
    $_requestResumeToken: true
}));
assert.eq(1, res.cursor.firstBatch.length);
assert(res.cursor.firstBatch[0].hasOwnProperty("$recordId"));
assert.hasFields(res.cursor, ["postBatchResumeToken"]);

const firstResumeToken = res.cursor.postBatchResumeToken;
const firstOplogBatch = res.cursor.firstBatch;

res = assert.commandWorked(
    localDb.runCommand({getMore: res.cursor.id, collection: oplogCollName, batchSize: 1}));
assert.eq(1, res.cursor.nextBatch.length);
assert(res.cursor.nextBatch[0].hasOwnProperty("$recordId"));
assert.hasFields(res.cursor, ["postBatchResumeToken"]);

const secondResumeToken = res.cursor.postBatchResumeToken;
const secondOplogBatch = res.cursor.nextBatch;

// Kill the cursor before attempting to resume.
assert.commandWorked(localDb.runCommand({killCursors: oplogCollName, cursors: [res.cursor.id]}));

// Try to resume the query from the saved resume token. This fails because '$_resumeAfter' expects a
// '$recordId' resume token, not a 'ts' resume token. This is appropriate since resuming on the
// oplog should occur by timestamp, not by record id.
assert.commandFailedWithCode(localDb.runCommand({
    find: oplogCollName,
    filter: {ts: {$gte: firstResumeToken.ts}},
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: firstResumeToken
}),
                             ErrorCodes.BadValue);

// Now try to resume using a '$recordId' resume token, which doesn't make a ton of sense, but is
// still allowed.
res = assert.commandWorked(localDb.runCommand({
    find: oplogCollName,
    filter: {ts: {$gte: firstResumeToken.ts}},
    hint: {$natural: 1},
    batchSize: 1,
    showRecordId: true,
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: firstOplogBatch[0].$recordId}
}));
assert.eq(1, res.cursor.firstBatch.length);
assert.eq(secondOplogBatch[0], res.cursor.firstBatch[0]);

// Kill the cursor before attempting to resume.
assert.commandWorked(localDb.runCommand({killCursors: oplogCollName, cursors: [res.cursor.id]}));

res = assert.commandWorked(localDb.runCommand({
    find: oplogCollName,
    filter: {ts: {$lte: secondResumeToken.ts}},
    hint: {$natural: 1},
    batchSize: 1,
    showRecordId: true,
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: firstOplogBatch[0].$recordId}
}));
assert.eq(1, res.cursor.firstBatch.length);
assert.eq(secondOplogBatch[0], res.cursor.firstBatch[0]);

// Kill the cursor before attempting to resume.
assert.commandWorked(localDb.runCommand({killCursors: oplogCollName, cursors: [res.cursor.id]}));

res = assert.commandWorked(localDb.runCommand({
    find: oplogCollName,
    filter: {ts: {$eq: secondResumeToken.ts}},
    hint: {$natural: 1},
    batchSize: 1,
    showRecordId: true,
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: firstOplogBatch[0].$recordId}
}));
assert.eq(1, res.cursor.firstBatch.length);
assert.eq(secondOplogBatch[0], res.cursor.firstBatch[0]);

// Kill the cursor before attempting to resume.
assert.commandWorked(localDb.runCommand({killCursors: oplogCollName, cursors: [res.cursor.id]}));

res = assert.commandWorked(localDb.runCommand({
    find: oplogCollName,
    filter: {ts: {$gt: firstResumeToken.ts, $lte: secondResumeToken.ts}},
    hint: {$natural: 1},
    batchSize: 1,
    showRecordId: true,
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: firstOplogBatch[0].$recordId}
}));
assert.eq(1, res.cursor.firstBatch.length);
assert.eq(secondOplogBatch[0], res.cursor.firstBatch[0]);

// Kill the cursor before attempting to resume.
assert.commandWorked(localDb.runCommand({killCursors: oplogCollName, cursors: [res.cursor.id]}));

// Try to resume the query from a non-existent recordId and check that it fails to position the
// cursor to the record specified in the resume token.
assert.commandFailedWithCode(localDb.runCommand({
    find: oplogCollName,
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: NumberLong("50")}
}),
                             ErrorCodes.KeyNotFound);

// When we have a predicate on a 'ts' field of the oplog collection, we can build an optimized
// collection scan. Make sure we can run such optimized scans and get the result.
assert.eq(oplogColl.find({ts: {$gte: firstResumeToken.ts}}).itcount(), 3);
assert.eq(oplogColl.find({ts: {$gt: firstResumeToken.ts}}).itcount(), 2);
assert.gt(oplogColl.find({ts: {$lte: firstResumeToken.ts}}).itcount(), 1);
assert.gt(oplogColl.find({ts: {$lt: firstResumeToken.ts}}).itcount(), 1);

rst.stopSet();
