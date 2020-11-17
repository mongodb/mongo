/*
 * Tests that reads that fail during rollback with a NotPrimaryError will replace their
 * "not master" error messages with "not primary" if the client sends "helloOk:true" as a part
 * of their isMaster request.
 *
 * In practice, drivers will send "helloOk: true" in the initial handshake when
 * opening a connection to the database.
 * @tags: [requires_majority_read_concern]
 */

(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "not_primary_errors_returned_during_rollback_if_helloOk";

// Set up Rollback Test.
let rollbackTest = new RollbackTest();

// Insert a document to be read later.
assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert({}));

let rollbackNode = rollbackTest.transitionToRollbackOperations();

setFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");

// Start rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

jsTestLog("Reconnecting to " + rollbackNode.host + " after rollback");
reconnect(rollbackNode.getDB(dbName));

// Wait for rollback to hang.
checkLog.contains(rollbackNode, "rollbackHangAfterTransitionToRollback fail point enabled.");

// Make sure we can't read during rollback. Since we want to exercise the real check for
// primary in the 'find' command, we have to disable the best-effort check for primary in service
// entry point.
setFailPoint(rollbackNode, "skipCheckingForNotPrimaryInCommandDispatch");
jsTestLog("Reading during rollback returns not master error message");
let res = assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand({"find": collName}),
                                       ErrorCodes.NotPrimaryOrSecondary,
                                       "find did not fail with NotPrimaryOrSecondary");

// Since we did not send "helloOk: true", the error message should include "not master or
// secondary".
assert(res.errmsg.includes("not master or secondary"), res);

// An isMaster response will not contain "helloOk: true" if the client does not send
// helloOk in the request.
res = assert.commandWorked(rollbackNode.getDB(dbName).adminCommand({isMaster: 1}));
assert.eq(res.helloOk, undefined);

// Run the isMaster command with "helloOk: true" on the secondary.
res = assert.commandWorked(rollbackNode.getDB(dbName).adminCommand({isMaster: 1, helloOk: true}));
// The response should contain "helloOk: true", which indicates to the client that the
// server supports the hello command.
assert.eq(res.helloOk, true);

jsTestLog("Reading during rollback returns not primary error message");
res = assert.commandFailedWithCode(rollbackNode.getDB(dbName).runCommand({"find": collName}),
                                   ErrorCodes.NotPrimaryOrSecondary,
                                   "find did not fail with NotPrimaryOrSecondary");
// Since we sent "helloOk: true", the error message should include "not primary or secondary".
assert(res.errmsg.includes("not primary or secondary"), res);
assert(!res.errmsg.includes("not master"), res);

clearFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");

rollbackTest.transitionToSteadyStateOperations();
rollbackTest.stop();
}());
