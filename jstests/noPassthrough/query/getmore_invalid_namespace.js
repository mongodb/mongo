// A get more exception caused by an invalid or unauthorized get more request does not cause
// the get more's ClientCursor to be destroyed.  This prevents an unauthorized user from
// improperly killing a cursor by issuing an invalid get more request.
// TODO SERVER-102285: Run this test against a sharded cluster as well.

const conn = MongoRunner.runMongod({});
const dbName = 'getmore_cmd_test';
const collName = 'getmore_cmd_invalid_namespace';
const db = conn.getDB(dbName);
const coll = db[collName];
coll.drop();

const numDocs = 10;
const initialBatchSize = 2;

assert.commandWorked(db.createCollection(collName));
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

let cmdRes;
// Run a find against the initial collection to establish a cursor.
cmdRes = db.runCommand({find: collName, batchSize: initialBatchSize});
jsTestLog(cmdRes);
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursor.firstBatch.length, initialBatchSize);
for (let i = 0; i < cmdRes.cursor.firstBatch.length; i++) {
    assert.eq(cmdRes.cursor.firstBatch[i]["a"], i);
}

let cursorId = cmdRes.cursor.id;
assert.neq(cursorId, 0);

// Now attempt a getMore with an invalid namespace.
cmdRes = db.runCommand({getMore: cursorId, collection: "invalid_namespace_for_getMore_test"});
jsTestLog(cmdRes);
assert.commandFailedWithCode(cmdRes, ErrorCodes.Unauthorized);

// Now use the cursor again on a valid namespace to get the rest of the data.
cmdRes = db.runCommand({getMore: cursorId, collection: collName});
jsTestLog(cmdRes);
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursor.id, 0);
assert.eq(cmdRes.cursor.nextBatch.length, numDocs - initialBatchSize);
for (let i = 0; i < cmdRes.cursor.nextBatch.length; i++) {
    assert.eq(cmdRes.cursor.nextBatch[i]["a"], i + initialBatchSize);
}

MongoRunner.stopMongod(conn);
