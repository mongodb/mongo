/**
 * Tests that tailable cursors work correctly across query yield and getMore commands.
 *
 * This test is trying to check that internal server cursors remain valid across yield and getMore
 * commands for the SBE query engine.
 */

(function() {
"use strict";

// Create a capped collection with max of 20 documents.
const mydb = db.getSiblingDB(jsTestName() + "_db");
const mycoll = mydb.getCollection(jsTestName() + "_coll");
mycoll.drop();
assert.commandWorked(mydb.createCollection(mycoll.getName(), {capped: true, size: 4096, max: 20}));

// Insert 10 documents.
const numDocs = 10;
for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(mycoll.insert({_id: i}));
}

// Set query to yield after every document read, so yielding is exercised at much as possible in the
// subsequent find and getMore commands.
const setParamResWas =
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}))
        .was;

try {
    assert.eq(numDocs % 2, 0);
    const batchSizeVal = numDocs / 2;

    // Fetch the first half of the documents with a find cmd.
    let cmdRes = mydb.runCommand({find: mycoll.getName(), batchSize: batchSizeVal, tailable: true});
    const originalCursorId = cmdRes.cursor.id;
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, mycoll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 5);

    // Fetch the next half of the documents with a getMore cmd.
    cmdRes = mydb.runCommand({getMore: cmdRes.cursor.id, collection: mycoll.getName()});
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, originalCursorId);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, mycoll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 5);

    // Try to fetch more documents, but find none.
    cmdRes = mydb.runCommand({getMore: cmdRes.cursor.id, collection: mycoll.getName()});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.id, originalCursorId);
    assert.eq(cmdRes.cursor.ns, mycoll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, 0);

    // Insert less than 'batchSizeVal' more documents to find with another getMore command.
    const nextBatchSize = batchSizeVal - 1;
    for (let i = numDocs; i < numDocs + nextBatchSize; ++i) {
        assert.commandWorked(mycoll.insert({_id: i}));
    }

    // Fetch all the remaining documents that can be found.
    cmdRes = mydb.runCommand({getMore: cmdRes.cursor.id, collection: mycoll.getName()});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.id, originalCursorId);
    assert.eq(cmdRes.cursor.ns, mycoll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length, nextBatchSize);
} finally {
    // We must ensure that the mongod's original settings are returned because this test shares a
    // common mongod instance with other tests.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: setParamResWas}));
}
})();
