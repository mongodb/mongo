/**
 * Test that a change stream pipeline which encounters a retryable exception responds to the client
 * with an error object that includes the "ResumableChangeStreamError" label.
 * @tags: [requires_replication, requires_journaling, uses_change_streams, requires_fcv_44]
 */
(function() {
"use strict";

// Create a two-node replica set so that we can issue a request to the Secondary.
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

// Disable "slaveOk" on the connection so that we are not allowed to run on the Secondary.
const testDB = rst.getSecondary().getDB(jsTestName());
testDB.getMongo().setSlaveOk(false);
const coll = testDB.test;

// Issue a change stream. We should fail with a NotMasterNoSlaveOk error.
const err = assert.throws(() => coll.watch([]));
assert.commandFailedWithCode(err, ErrorCodes.NotMasterNoSlaveOk);

// Confirm that the response includes the "ResumableChangeStreamError" error label.
assert("errorLabels" in err, err);
assert.contains("ResumableChangeStreamError", err.errorLabels, err);

rst.stopSet();
}());