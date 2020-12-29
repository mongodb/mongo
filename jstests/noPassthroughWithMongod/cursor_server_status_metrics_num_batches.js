/**
 * Tests for serverStatus metrics.cursor.totalOpened and metrics.cursor.moreThanOneBatch.
 */
(function() {
"use strict";
const coll = db[jsTest.name()];
coll.drop();

function getTotalCursorsOpened() {
    return db.serverStatus().metrics.cursor.totalOpened;
}

function getNumCursorsWithMoreThanOneBatch() {
    return db.serverStatus().metrics.cursor.moreThanOneBatch;
}

const initialTotalOpened = getTotalCursorsOpened();
const initialNumCursorsWithMoreThanOneBatch = getNumCursorsWithMoreThanOneBatch();
const initialCurrentlyOpen = db.serverStatus().metrics.cursor.open.total;

for (let i = 0; i < 20; i++) {
    assert.commandWorked(coll.insert({a: i, b: i * 2, c: i - 1, d: "metanoia", e: "another"}));
}

{
    // Open a cursor with batchSize 2.
    const cursor1 = coll.find().batchSize(2);

    // Get 1st document. The next batch should not have been fetched. (This actually opens the
    // cursor.)
    cursor1.next();
    assert.eq(initialTotalOpened + 1, getTotalCursorsOpened());

    // Kill the cursor to update the metric. It shouldn't change, as this cursor didn't fetch a new
    // batch.
    assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [cursor1.getId()]}));
    assert.eq(initialNumCursorsWithMoreThanOneBatch, getNumCursorsWithMoreThanOneBatch());
}

// Test cursors that have overlapping lives.
{
    // Open another cursor with batchSize 2, and then run through two batches.
    const cursor2 = coll.find().batchSize(2);
    for (let i = 0; i < 4; i++) {
        cursor2.next();

        assert.eq(initialTotalOpened + 2, getTotalCursorsOpened());
        // Can't see the update until after the cursor is dead.
        assert.eq(initialNumCursorsWithMoreThanOneBatch, getNumCursorsWithMoreThanOneBatch());
    }

    // Open another cursor for the aggregate command.
    const cursor3 = coll.aggregate([], {cursor: {batchSize: 2}});

    assert.eq(initialTotalOpened + 3, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch, getNumCursorsWithMoreThanOneBatch());

    assert.eq(initialTotalOpened + 3, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch, getNumCursorsWithMoreThanOneBatch());

    // Exhaust cursor2, terminating it and updating the metric.
    while (cursor2.hasNext()) {
        cursor2.next();
    }
    assert.eq(initialTotalOpened + 3, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch + 1, getNumCursorsWithMoreThanOneBatch());

    // Get one batch of aggregate.
    for (let i = 0; i < 3; i++) {
        cursor3.next();
    }
    assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [cursor3.getId()]}));
    assert.eq(initialTotalOpened + 3, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch + 2, getNumCursorsWithMoreThanOneBatch());
}

{
    // This cursor will not fetch the next batch.
    const cursor4 = coll.aggregate([], {cursor: {batchSize: 2}});
    for (let i = 0; i < 2; i++) {
        cursor4.next();
    }
    assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [cursor4.getId()]}));
    assert.eq(initialTotalOpened + 4, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch + 2, getNumCursorsWithMoreThanOneBatch());
}

// Test listIndex cursors.
{
    jsTestLog("Create listIndex cursors and test without and withGetMore");
    // Create a few indexes, to test the listIndex command.
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: 1}));
    assert.commandWorked(coll.createIndex({d: 1}));

    // This cursor will not be used with a getMore.
    const listIndCursorId1 =
        assert.commandWorked(db.runCommand({listIndexes: coll.getName(), cursor: {batchSize: 2}}))
            .cursor.id;
    assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [listIndCursorId1]}));
    assert.eq(initialTotalOpened + 5, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch + 2, getNumCursorsWithMoreThanOneBatch());

    // Use this cursor with a getMore.
    const listIndCursorId2 =
        assert.commandWorked(db.runCommand({listIndexes: coll.getName(), cursor: {batchSize: 2}}))
            .cursor.id;
    assert.commandWorked(db.runCommand({getMore: listIndCursorId2, collection: coll.getName()}));
    assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [listIndCursorId2]}));
    assert.eq(initialTotalOpened + 6, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch + 3, getNumCursorsWithMoreThanOneBatch());
}

// Test listCollections cursors.
{
    jsTestLog("Creating new collections to test listCollections cursor behavior.");
    let collections = [];
    for (let i = 0; i < 3; i++) {
        collections.push("test_collection_" + jsTest.name() + "_" + i);
        db[collections[i]].drop();
        db.createCollection(collections[i]);
    }

    // Create cursors to test the listCollections command.
    const listCollsCursorId1 =
        assert.commandWorked(db.runCommand({listCollections: 1, cursor: {batchSize: 2}})).cursor.id;
    assert.commandWorked(
        db.runCommand({killCursors: "$cmd.listCollections", cursors: [listCollsCursorId1]}));
    assert.eq(initialTotalOpened + 7, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch + 3, getNumCursorsWithMoreThanOneBatch());

    // Use this cursor with a getMore.
    const listCollsCursorId2 =
        assert.commandWorked(db.runCommand({listCollections: 1, cursor: {batchSize: 2}})).cursor.id;
    assert.commandWorked(
        db.runCommand({getMore: listCollsCursorId2, collection: "$cmd.listCollections"}));
    assert.commandWorked(
        db.runCommand({killCursors: "$cmd.listCollections", cursors: [listCollsCursorId2]}));
    assert.eq(initialTotalOpened + 8, getTotalCursorsOpened());
    assert.eq(initialNumCursorsWithMoreThanOneBatch + 4, getNumCursorsWithMoreThanOneBatch());

    for (let i = 0; i < 3; i++) {
        db[collections[i]].drop();
    }
}

// Cursors this test opened should be closed.
assert.eq(initialCurrentlyOpen, db.serverStatus().metrics.cursor.open.total);
}());
