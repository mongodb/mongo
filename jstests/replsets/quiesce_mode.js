/**
 * Tests the behavior of quiesce mode: the period during secondary shutdown where existing
 * operations are allowed to continue and new operations are accepted, but hello requests return
 * a ShutdownInProgress error, so that clients begin routing operations elsewhere.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({
    name: "quiesce_mode",
    nodes: 2,
    nodeOptions: {setParameter: "shutdownTimeoutMillisForSignaledShutdown=5000"},
});
replTest.startSet();
replTest.initiate();

const dbName = "test";
const collName = "coll";
const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);
assert.commandWorked(primaryDB.coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}], {writeConcern: {w: "majority"}}));

function checkTopologyVersion(res, topologyVersionField) {
    assert(res.hasOwnProperty("topologyVersion"), res);
    assert.eq(res.topologyVersion.counter, topologyVersionField.counter + 1);
}

function checkRemainingQuiesceTime(res) {
    assert(res.hasOwnProperty("remainingQuiesceTimeMillis"), res);
}

function runAwaitableHello(topologyVersionField) {
    let res = assert.commandFailedWithCode(
        db.runCommand({
            hello: 1,
            topologyVersion: topologyVersionField,
            maxAwaitTimeMS: 99999999,
        }),
        ErrorCodes.ShutdownInProgress,
    );
    assert(res.hasOwnProperty("topologyVersion"), res);
    assert(res.hasOwnProperty("remainingQuiesceTimeMillis"), res);
    assert.eq(res.topologyVersion.counter, topologyVersionField.counter + 1);
}

function runFind() {
    db.getMongo().setSecondaryOk();
    assert.eq(4, db.getSiblingDB("test").coll.find().itcount());
}

jsTestLog("Test quiesce mode when shutting down a secondary");

jsTestLog("Create a cursor on the secondary.");
replTest.awaitReplication();
let res = assert.commandWorked(secondaryDB.runCommand({find: collName, batchSize: 2}));
assert.eq(2, res.cursor.firstBatch.length, res);
let cursorId = res.cursor.id;

jsTestLog("Create a hanging operation on the secondary.");
const fpData = {
    nss: dbName + "." + collName,
};
let findCmdFailPoint = configureFailPoint(secondary, "waitInFindBeforeMakingBatch", fpData);
let findCmd = startParallelShell(runFind, secondary.port);
findCmdFailPoint.wait();

jsTestLog("Create a hanging hello on the secondary.");
res = assert.commandWorked(secondary.adminCommand({hello: 1}));
assert(res.hasOwnProperty("topologyVersion"), res);
let topologyVersionField = res.topologyVersion;
let helloFailPoint = configureFailPoint(secondary, "waitForHelloResponse");
let hello = startParallelShell(funWithArgs(runAwaitableHello, topologyVersionField), secondary.port);
helloFailPoint.wait();
assert.eq(1, secondary.getDB("admin").serverStatus().connections.awaitingTopologyChanges);

jsTestLog("Transition the secondary to quiesce mode.");
let quiesceModeFailPoint = configureFailPoint(secondary, "hangDuringQuiesceMode");
// We must skip validation due to the failpoint that hangs find commands.
replTest.stop(secondary, null /*signal*/, {skipValidation: true}, {forRestart: true, waitpid: false});
quiesceModeFailPoint.wait();

jsTestLog("The waiting hello returns a ShutdownInProgress error.");
hello();
// We cannot check the metrics because serverStatus returns ShutdownInProgress.
assert.commandFailedWithCode(secondaryDB.adminCommand({serverStatus: 1}), ErrorCodes.ShutdownInProgress);

jsTestLog("New hello commands return a ShutdownInProgress error.");
res = assert.commandFailedWithCode(secondary.adminCommand({hello: 1}), ErrorCodes.ShutdownInProgress);
checkTopologyVersion(res, topologyVersionField);
checkRemainingQuiesceTime(res);

res = assert.commandFailedWithCode(
    secondary.adminCommand({
        hello: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }),
    ErrorCodes.ShutdownInProgress,
);

checkTopologyVersion(res, topologyVersionField);
checkRemainingQuiesceTime(res);

// Test operation behavior during quiesce mode.
jsTestLog("The running operation is allowed to finish.");
findCmdFailPoint.off();
findCmd();

jsTestLog("getMores on existing cursors are allowed.");
res = assert.commandWorked(secondaryDB.runCommand({getMore: cursorId, collection: collName}));
assert.eq(2, res.cursor.nextBatch.length, res);

jsTestLog("New operations are allowed.");
assert.eq(4, secondaryDB.coll.find().itcount());

jsTestLog("Let shutdown progress to start killing operations.");
let pauseWhileKillingOperationsFailPoint = configureFailPoint(secondary, "pauseWhileKillingOperationsAtShutdown");
quiesceModeFailPoint.off();

// The waitForFailPoint command can fail with InterruptedAtShutdown if killed by the shutdown.
pauseWhileKillingOperationsFailPoint.wait({expectedErrorCodes: [ErrorCodes.InterruptedAtShutdown]});

jsTestLog("Operations fail with a shutdown error and append the topologyVersion.");
checkTopologyVersion(
    assert.commandFailedWithCode(secondaryDB.runCommand({find: collName}), ErrorCodes.InterruptedAtShutdown),
    topologyVersionField,
);

jsTestLog("Restart the secondary.");
replTest.restart(secondary);
replTest.awaitSecondaryNodes();

jsTestLog("Test quiesce mode when shutting down a primary.");

jsTestLog("Create a cursor on the primary.");
res = assert.commandWorked(primaryDB.runCommand({find: collName, batchSize: 2}));
assert.eq(2, res.cursor.firstBatch.length, res);
cursorId = res.cursor.id;

jsTestLog("Create a hanging operation on the primary.");
// We must use a different failpoint on the primary that will not pause the command with locks held,
// since the stepdown takes an RSTL X lock.
findCmdFailPoint = configureFailPoint(primary, "waitAfterCommandFinishesExecution", {
    ns: dbName + "." + collName,
    commands: ["find"],
});
findCmd = startParallelShell(runFind, primary.port);
findCmdFailPoint.wait();

jsTestLog("Hang the primary in shutdown after stepdown.");
let postStepdownFailpoint = configureFailPoint(primary, "hangInShutdownAfterStepdown");
// We must skip validation due to the failpoint that hangs find commands.
replTest.stop(primary, null /*signal*/, {skipValidation: true}, {forRestart: true, waitpid: false});
postStepdownFailpoint.wait();

jsTestLog("Create a hanging hello on the primary.");
res = assert.commandWorked(primary.adminCommand({hello: 1}));
assert(res.hasOwnProperty("topologyVersion"), res);
topologyVersionField = res.topologyVersion;
helloFailPoint = configureFailPoint(primary, "waitForHelloResponse");
hello = startParallelShell(funWithArgs(runAwaitableHello, topologyVersionField), primary.port);
helloFailPoint.wait();
assert.eq(1, primary.getDB("admin").serverStatus().connections.awaitingTopologyChanges);

jsTestLog("Transition the primary to quiesce mode.");
quiesceModeFailPoint = configureFailPoint(primary, "hangDuringQuiesceMode");
postStepdownFailpoint.off();
quiesceModeFailPoint.wait();

jsTestLog("The waiting hello returns a ShutdownInProgress error.");
hello();
// We cannot check the metrics because serverStatus returns ShutdownInProgress.
assert.commandFailedWithCode(primaryDB.adminCommand({serverStatus: 1}), ErrorCodes.ShutdownInProgress);

jsTestLog("New hello commands return a ShutdownInProgress error.");
res = assert.commandFailedWithCode(primary.adminCommand({hello: 1}), ErrorCodes.ShutdownInProgress);
checkTopologyVersion(res, topologyVersionField);
checkRemainingQuiesceTime(res);

res = assert.commandFailedWithCode(
    primary.adminCommand({
        hello: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }),
    ErrorCodes.ShutdownInProgress,
);

checkTopologyVersion(res, topologyVersionField);
checkRemainingQuiesceTime(res);

// Test operation behavior during quiesce mode.
jsTestLog("The running operation is allowed to finish.");
findCmdFailPoint.off();
findCmd();

jsTestLog("getMores on existing cursors are allowed.");
res = assert.commandWorked(primaryDB.runCommand({getMore: cursorId, collection: collName}));
assert.eq(2, res.cursor.nextBatch.length, res);

jsTestLog("New operations are allowed.");
assert.eq(4, primaryDB.coll.find().itcount());

jsTestLog("Let shutdown progress to start killing operations.");
pauseWhileKillingOperationsFailPoint = configureFailPoint(primary, "pauseWhileKillingOperationsAtShutdown");
quiesceModeFailPoint.off();
// The waitForFailPoint command can fail with InterruptedAtShutdown if killed by the shutdown.
pauseWhileKillingOperationsFailPoint.wait({expectedErrorCodes: [ErrorCodes.InterruptedAtShutdown]});

jsTestLog("Operations fail with a shutdown error and append the topologyVersion.");
checkTopologyVersion(
    assert.commandFailedWithCode(primaryDB.runCommand({find: collName}), ErrorCodes.InterruptedAtShutdown),
    topologyVersionField,
);

replTest.stopSet();
