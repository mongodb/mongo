/**
 * Verify that the 'TestData.allowUncleanShutdowns' parameter is correctly obeyed in RollbackTest.
 * We test that nodes can only be shut down with the SIGTERM signal (15), not SIGKILL (9), when
 * allowUncleanShutdowns=false.
 *
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");

TestData.rollbackShutdowns = true;
// Only clean shutdowns should be allowed.
TestData.allowUncleanShutdowns = false;

let dbName = "test";
let collName = "coll";

// Execute a simple rollback. The specific documents are unimportant.
let rollbackTest = new RollbackTest(jsTestName());
assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert({}));
let rollbackNode = rollbackTest.transitionToRollbackOperations();
assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({}));

// Neither of these should be allowed to shut down the node uncleanly.
const SIGKILL = 9;
rollbackTest.restartNode(1, SIGKILL);
rollbackTest.restartNode(1, SIGKILL);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Make sure no unclean shutdowns occurred.
assert.eq(rawMongoProgramOutput().search(/Detected unclean shutdown/), -1);

rollbackTest.stop();
}());
