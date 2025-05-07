// @tags: [
//   requires_getmore,
//   # This test relies on query commands returning specific batch-sized responses.
//   assumes_no_implicit_cursor_exhaustion,
// ]

// Tests that explicitly invoke the find and getMore commands.
var cmdRes;
var cursorId;
var collName = 'find_getmore_cmd';
var coll = db[collName];

const findCommandBatchSize = assert.commandWorked(db.adminCommand(
    {getParameter: 1, internalQueryFindCommandBatchSize: 1}))["internalQueryFindCommandBatchSize"];

// The limit should be less than the find command batch size but at least 1.
const kLimit = Math.min(10, Math.max(1, findCommandBatchSize - 1));

coll.drop();
for (var i = 0; i < findCommandBatchSize + 50; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Verify result of a find command that specifies none of the optional arguments.
cmdRes = db.runCommand({find: collName});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, findCommandBatchSize);

// Use a getMore command to get the next batch.
cursorId = cmdRes.cursor.id;
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.nextBatch.length, 50);

// Cursor should have been closed, so attempting to get another batch should fail.
cmdRes = db.runCommand({getMore: cursorId, collection: collName});
assert.commandFailed(cmdRes);

// Find command with limit.
cmdRes = db.runCommand({find: collName, limit: kLimit});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, kLimit);
if (kLimit < findCommandBatchSize) {
    assert.eq(cmdRes.cursor.id, NumberLong(0));
}

// Find command with positive batchSize followed by getMore command with positive batchSize.
cmdRes = db.runCommand({find: collName, batchSize: 10});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 10);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, batchSize: NumberInt(5)});
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.nextBatch.length, 5);

// Find command with zero batchSize followed by getMore command (default batchSize).
cmdRes = db.runCommand({find: collName, batchSize: 0});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 0);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, batchSize: NumberInt(5)});
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.nextBatch.length, 5);

// Batch size and limit together.
cmdRes = db.runCommand({find: collName, batchSize: 10, limit: 20});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 10);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName, batchSize: NumberInt(11)});
assert.eq(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.nextBatch.length, 10);

// Find command with batchSize and singleBatch.
cmdRes = db.runCommand({find: collName, batchSize: 10, singleBatch: true});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 10);

// Error on invalid collection name.
assert.commandFailedWithCode(db.runCommand({find: ""}), ErrorCodes.InvalidNamespace);
