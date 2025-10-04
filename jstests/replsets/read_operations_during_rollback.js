/**
 * This test makes sure 'find' and 'getMore' commands fail correctly during rollback.
 *
 * @tags: [
 *   requires_majority_read_concern,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";
import {reconnect} from "jstests/replsets/rslib.js";

const dbName = "test";
const collName = "coll";

// Set up Rollback Test.
let rollbackTest = new RollbackTest();

// Insert documents to be read later.
assert.commandWorked(rollbackTest.getPrimary().getDB(dbName)[collName].insert([{}, {}, {}]));

let rollbackNode = rollbackTest.transitionToRollbackOperations();

// Open a cursor on 'rollbackNode' which returns partial results, but will remain open and idle
// during the rollback process.
const findCmdRes = assert.commandWorked(rollbackNode.getDB(dbName).runCommand({"find": collName, batchSize: 2}));
assert.eq(2, findCmdRes.cursor.firstBatch.length, findCmdRes);
const idleCursorId = findCmdRes.cursor.id;
assert.neq(0, idleCursorId, findCmdRes);

const failPointAfterTransition = configureFailPoint(rollbackNode, "rollbackHangAfterTransitionToRollback");
const failPointAfterPinCursor = configureFailPoint(rollbackNode, "getMoreHangAfterPinCursor");

const joinGetMoreThread = startParallelShell(() => {
    db.getMongo().setSecondaryOk();
    const cursorID = assert.commandWorked(db.runCommand({"find": "coll", batchSize: 0})).cursor.id;
    // Make sure an outstanding read operation gets killed during rollback even though the read
    // was started before rollback. Outstanding read operations are killed during rollback and
    // their connections are closed shortly after. So we would get either an error
    // (InterruptedDueToReplStateChange) if the error message is sent out and received before
    // the connection is closed or a network error exception.
    try {
        assert.commandFailedWithCode(
            db.runCommand({"getMore": cursorID, collection: "coll"}),
            ErrorCodes.InterruptedDueToReplStateChange,
        );
    } catch (e) {
        assert.includes(e.toString(), "network error while attempting to run command");
    }
}, rollbackNode.port);

const cursorIdToBeReadDuringRollback = assert.commandWorked(
    rollbackNode.getDB(dbName).runCommand({"find": collName, batchSize: 0}),
).cursor.id;

// Wait for 'getMore' to hang on the test collection.
assert.soonNoExcept(() => {
    const filter = {"command.getMore": {$exists: true}, "command.collection": collName};
    return rollbackNode.getDB(dbName).adminCommand("currentOp", filter).inprog.length === 1;
});

// Start rollback.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

jsTestLog("Reconnecting to " + rollbackNode.host + " after rollback");
reconnect(rollbackNode.getDB(dbName));

// Wait for rollback to hang. We continuously retry the wait command since the rollback node
// might reject new connections initially, causing the command to fail.
assert.soon(() => {
    try {
        failPointAfterTransition.wait();
        return true;
    } catch (e) {
        return false;
    }
});

failPointAfterPinCursor.off();

jsTestLog("Wait for 'getMore' thread to join.");
joinGetMoreThread();

jsTestLog("Reading during rollback.");
// Make sure that read operations fail during rollback.
assert.commandFailedWithCode(
    rollbackNode.getDB(dbName).runCommand({"find": collName}),
    ErrorCodes.NotPrimaryOrSecondary,
);
assert.commandFailedWithCode(
    rollbackNode.getDB(dbName).runCommand({"getMore": cursorIdToBeReadDuringRollback, collection: collName}),
    ErrorCodes.NotPrimaryOrSecondary,
);

// Disable the best-effort check for primary-ness in the service entry point, so that we
// exercise the real check for primary-ness in 'find' and 'getMore' commands.
configureFailPoint(rollbackNode, "skipCheckingForNotPrimaryInCommandDispatch");

jsTestLog("Reading during rollback (again with command dispatch checks disabled).");
assert.commandFailedWithCode(
    rollbackNode.getDB(dbName).runCommand({"find": collName}),
    ErrorCodes.NotPrimaryOrSecondary,
);
assert.commandFailedWithCode(
    rollbackNode.getDB(dbName).runCommand({"getMore": cursorIdToBeReadDuringRollback, collection: collName}),
    ErrorCodes.NotPrimaryOrSecondary,
);

failPointAfterTransition.off();

rollbackTest.transitionToSteadyStateOperations();

const replMetrics = assert.commandWorked(rollbackNode.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "rollback");
// TODO (SERVER-85259): Remove references to replMetrics.stateTransition.userOperations*
assert(
    replMetrics.stateTransition.totalOperationsRunning || replMetrics.stateTransition.userOperationsRunning,
    () =>
        "Response should have a 'stateTransition.totalOperationsRunning' or 'stateTransition.userOperationsRunning' (bin <= 7.2) field: " +
        tojson(replMetrics),
);
assert(
    replMetrics.stateTransition.totalOperationsKilled || replMetrics.stateTransition.userOperationsKilled,
    () =>
        "Response should have a 'stateTransition.totalOperationsKilled' or 'stateTransition.userOperationsKilled' (bin <= 7.2) field: " +
        tojson(replMetrics),
);

// Run a getMore against the idle cursor that remained open throughout the rollback. The getMore
// should fail since the cursor has been invalidated by the rollback.
assert.commandFailedWithCode(
    rollbackNode.getDB(dbName).runCommand({"getMore": idleCursorId, collection: collName}),
    ErrorCodes.QueryPlanKilled,
);

// Check the replica set.
rollbackTest.stop();
