/**
 * Test that 'failGetMoreAfterCursorCheckout' works.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB(jsTestName());
const coll = testDB.test;

// Insert a set of test documents into the collection.
for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({_id: i}));
}

// Perform the test for both 'find' and 'aggregate' cursors.
for (let testCursor of [coll.find({}).sort({_id: 1}).batchSize(2),
                        coll.aggregate([{$sort: {_id: 1}}], {cursor: {batchSize: 2}})]) {
    // Activate the failpoint and set the exception that it will throw.
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failGetMoreAfterCursorCheckout",
        mode: "alwaysOn",
        data: {"errorCode": ErrorCodes.ShutdownInProgress}
    }));

    // Consume the documents from the first batch, leaving the cursor open.
    assert.docEq(testCursor.next(), {_id: 0});
    assert.docEq(testCursor.next(), {_id: 1});
    assert.eq(testCursor.objsLeftInBatch(), 0);

    // Issue a getMore and confirm that the failpoint throws the expected exception.
    const getMoreRes = assert.throws(() => testCursor.hasNext() && testCursor.next());
    assert.commandFailedWithCode(getMoreRes, ErrorCodes.ShutdownInProgress);

    // Disable the failpoint.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "failGetMoreAfterCursorCheckout", mode: "off"}));
}

rst.stopSet();
}());
