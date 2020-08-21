// @tags: [
//   requires_capped,
//   requires_getmore,
//   sbe_incompatible,
// ]

// Tests for the behavior of combining the tailable and awaitData options to the getMore command
// with the batchSize option.
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.isMongos().

const collName = "tailable_getmore_batch_size";
const coll = db[collName];
const batchSize = 2;

function dropAndRecreateColl({numDocs}) {
    coll.drop();
    assert.commandWorked(db.createCollection(collName, {capped: true, size: 1024}));
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());
}

// Test that running a find with the 'tailable' option will return results immediately, even if
// there are fewer than the specified batch size.
dropAndRecreateColl({numDocs: batchSize - 1});
let findRes =
    assert.commandWorked(db.runCommand({find: collName, tailable: true, batchSize: batchSize}));
assert.eq(findRes.cursor.firstBatch.length, batchSize - 1);
assert.neq(findRes.cursor.id, 0);
// Test that the same is true for a find with the 'tailable' and 'awaitData' options set.
findRes = assert.commandWorked(
    db.runCommand({find: collName, tailable: true, awaitData: true, batchSize: batchSize}));
assert.eq(findRes.cursor.firstBatch.length, batchSize - 1);
assert.neq(findRes.cursor.id, 0);

/**
 * Runs a find command with a batchSize of 'batchSize' to establish a cursor. Asserts that the
 * command worked and that the cursor id is not 0, then returns the cursor id.
 */
function openCursor({batchSize, tailable, awaitData}) {
    const findRes = assert.commandWorked(db.runCommand(
        {find: collName, tailable: tailable, awaitData: awaitData, batchSize: batchSize}));
    assert.eq(findRes.cursor.firstBatch.length, batchSize);
    assert.neq(findRes.cursor.id, 0);
    assert.eq(findRes.cursor.ns, coll.getFullName());
    return findRes.cursor.id;
}

// Test that specifying a batch size to a getMore on a tailable cursor produces a batch of the
// desired size when the number of results is larger than the batch size.

// One batch's worth for the find and one more than one batch's worth for the getMore.
dropAndRecreateColl({numDocs: batchSize + (batchSize + 1)});
let cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: false});
let getMoreRes = assert.commandWorked(
    db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);

// Test that the same is true for a tailable, *awaitData* cursor when not running against
// mongos. Mongos may return empty batches for tailable + awaitData cursors if its awaitData
// timeout expires before it has received results from the shards.
if (!FixtureHelpers.isMongos(db)) {
    cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: true});
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);
}

// Test that specifying a batch size to a getMore on a tailable cursor returns all
// new results immediately, even if the batch size is larger than the number of new results.
// One batch's worth for the find and one less than one batch's worth for the getMore.
dropAndRecreateColl({numDocs: batchSize + (batchSize - 1)});
cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: false});
getMoreRes = assert.commandWorked(
    db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
assert.eq(getMoreRes.cursor.nextBatch.length, batchSize - 1);

// Test that the same is true for a tailable, *awaitData* cursor when run directly against
// mongod. Mongos may return empty batches for tailable + awaitData cursors if its awaitData
// timeout expires before it has received results from the shards.
if (!FixtureHelpers.isMongos(db)) {
    cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: true});
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize - 1);
}

// Test that using a smaller batch size than there are results will return all results without
// empty batches in between (SERVER-30799).
function checkNoIntermediateEmptyBatchesWhenBatchSizeSmall(awaitData) {
    dropAndRecreateColl({numDocs: batchSize * 3});
    cursorId = openCursor({batchSize: batchSize, tailable: true, awaitData: awaitData});
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, batchSize);
    getMoreRes = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: collName, batchSize: batchSize}));
    assert.eq(getMoreRes.cursor.nextBatch.length, 0);

    // Avoid leaving the cursor open. Cursors above are killed by drops, but we'll avoid dropping
    // the collection at the end so other consistency checks like validate can be run against it.
    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [getMoreRes.cursor.id]}));
}

checkNoIntermediateEmptyBatchesWhenBatchSizeSmall(false);

// When using a tailable cursor with a smaller batch size than there are results will *generally*
// return all results with no empty batches in between. However, this is not guaranteed when
// using a tailable + awaitData cursor against mongos, as mongos may return an empty batch if
// its awaitData timeout expires before it has received results from the shards.
if (!FixtureHelpers.isMongos(db)) {
    checkNoIntermediateEmptyBatchesWhenBatchSizeSmall(true);
}
}());
