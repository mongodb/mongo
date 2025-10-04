/*
 * This test makes sure the 'validate' command fails correctly during rollback.
 */
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = "test";
const collName = "coll";

// Set up Rollback Test.
let rollbackTest = new RollbackTest();

let rollbackNode = rollbackTest.transitionToRollbackOperations();

assert.commandWorked(
    rollbackNode.adminCommand({configureFailPoint: "rollbackHangAfterTransitionToRollback", mode: "alwaysOn"}),
);

// Start rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// Wait for rollback to hang.
checkLog.contains(rollbackNode, "rollbackHangAfterTransitionToRollback fail point enabled.");

// Try to run the validate command on the rollback node. This should fail with a
// NotPrimaryOrSecondary error.
const result = assert.commandFailedWithCode(
    rollbackNode.getDB(dbName).runCommand({"validate": collName}),
    ErrorCodes.NotPrimaryOrSecondary,
);
jsTestLog("Validation failed on rollback node as expected: " + tojson(result));

assert.commandWorked(
    rollbackNode.adminCommand({configureFailPoint: "rollbackHangAfterTransitionToRollback", mode: "off"}),
);

rollbackTest.transitionToSteadyStateOperations();

// Check the replica set.
rollbackTest.stop();
