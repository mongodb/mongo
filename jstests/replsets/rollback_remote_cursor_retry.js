/**
 * This tests that rollback has the ability to retry the 'find' command on the sync source's
 * oplog, which may fail due to transient network errors. It uses the 'failCommand' failpoint
 * to simulate exactly two network failures, so that common point resolution can succeed on the
 * third attempt.
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/replsets/libs/rollback_test.js");

const testName = "rollback_remote_cursor_retry";
const dbName = testName;

const rollbackTest = new RollbackTest(testName);

const replSet = rollbackTest.getTestFixture();

replSet.awaitReplication();

const rollbackNode = rollbackTest.transitionToRollbackOperations();
const syncSource = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

// This failpoint is used to make sure that we have started rollback before turning on
// 'failCommand'. Otherwise, we would be failing the 'find' command that we issue against
// the sync source before we decide to go into rollback.
const rollbackHangBeforeStartFailPoint =
    configureFailPoint(rollbackNode, "rollbackHangBeforeStart");

rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// Ensure that we've hit the failpoint before moving on.
rollbackHangBeforeStartFailPoint.wait();

// Fail the 'find' command exactly twice.
jsTestLog("Failing the next two 'find' commands.");
configureFailPoint(syncSource,
                   "failCommand",
                   {errorCode: 279, failInternalCommands: true, failCommands: ["find"]},
                   {times: 2});

// Let rollback proceed.
rollbackHangBeforeStartFailPoint.off();

rollbackTest.transitionToSteadyStateOperations();
rollbackTest.stop();
})();
