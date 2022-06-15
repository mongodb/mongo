/**
 * Test that a change stream pipeline which encounters a retryable exception responds to the client
 * with an error object that includes the "ResumableChangeStreamError" label.
 * @tags: [
 *   requires_replication,
 *   uses_change_streams,
 * ]
 */
(function() {
"use strict";

// Create a two-node replica set so that we can issue a request to the Secondary.
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

// Disable "secondaryOk" on the connection so that we are not allowed to run on the Secondary.
const testDB = rst.getSecondary().getDB(jsTestName());
testDB.getMongo().setSecondaryOk(false);
const coll = testDB.test;

// Issue a change stream. We should fail with a NotPrimaryNoSecondaryOk error.
const err = assert.throws(() => coll.watch([]));
assert.commandFailedWithCode(err, ErrorCodes.NotPrimaryNoSecondaryOk);

// Confirm that the response includes the "ResumableChangeStreamError" error label.
assert("errorLabels" in err, err);
assert.contains("ResumableChangeStreamError", err.errorLabels, err);

// Now verify that the 'failGetMoreAfterCursorCheckout' failpoint can effectively exercise the
// error label generation logic for change stream getMores.
function testFailGetMoreAfterCursorCheckoutFailpoint({errorCode, expectedLabel}) {
    // Re-enable "secondaryOk" on the test connection.
    testDB.getMongo().setSecondaryOk();

    // Activate the failpoint and set the exception that it will throw.
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "failGetMoreAfterCursorCheckout",
        mode: "alwaysOn",
        data: {"errorCode": errorCode}
    }));

    // Now open a valid $changeStream cursor...
    const aggCmdRes = assert.commandWorked(
        coll.runCommand("aggregate", {pipeline: [{$changeStream: {}}], cursor: {}}));

    // ... run a getMore using the cursorID from the original command response, and confirm that the
    // expected error was thrown...
    const getMoreRes = assert.commandFailedWithCode(
        testDB.runCommand({getMore: aggCmdRes.cursor.id, collection: coll.getName()}), errorCode);

    /// ... and confirm that the label is present or absent depending on the "expectedLabel" value.
    const errorLabels = (getMoreRes.errorLabels || []);
    assert.eq("errorLabels" in getMoreRes, expectedLabel, getMoreRes);
    assert.eq(errorLabels.includes("ResumableChangeStreamError"), expectedLabel, getMoreRes);

    // Finally, disable the failpoint.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "failGetMoreAfterCursorCheckout", mode: "off"}));
}
// Test the expected output for both resumable and non-resumable error codes.
testFailGetMoreAfterCursorCheckoutFailpoint(
    {errorCode: ErrorCodes.ShutdownInProgress, expectedLabel: true});
testFailGetMoreAfterCursorCheckoutFailpoint(
    {errorCode: ErrorCodes.FailedToParse, expectedLabel: false});

rst.stopSet();
}());
