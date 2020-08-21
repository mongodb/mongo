/**
 * Tests the behavior of quiesce mode: the period during secondary shutdown where existing
 * operations are allowed to continue and new operations are accepted, but isMaster requests return
 * a ShutdownInProgress error, so that clients begin routing operations elsewhere.
 * @tags: [requires_fcv_47]
 */
(function() {
"use strict";
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

const replTest = new ReplSetTest({
    name: "quiesce_mode",
    nodes: 2,
    nodeOptions: {setParameter: "shutdownTimeoutMillisForSignaledShutdown=5000"}
});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();

const dbName = "test";
const collName = "coll";
const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);
assert.commandWorked(primaryDB.coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}],
                                           {writeConcern: {w: "majority"}}));

function checkTopologyVersion(res, topologyVersionField) {
    assert(res.hasOwnProperty("topologyVersion"), res);
    assert.eq(res.topologyVersion.counter, topologyVersionField.counter + 1);
}

function checkRemainingQuiesceTime(res) {
    assert(res.hasOwnProperty("remainingQuiesceTimeMillis"), res);
}

function runAwaitableIsMaster(topologyVersionField) {
    let res = assert.commandFailedWithCode(db.runCommand({
        isMaster: 1,
        topologyVersion: topologyVersionField,
        maxAwaitTimeMS: 99999999,
    }),
                                           ErrorCodes.ShutdownInProgress);
    assert(res.hasOwnProperty("topologyVersion"), res);
    assert(res.hasOwnProperty("remainingQuiesceTimeMillis"), res);
    assert.eq(res.topologyVersion.counter, topologyVersionField.counter + 1);
}

function runFind() {
    db.getMongo().setSlaveOk();
    assert.eq(4, db.getSiblingDB("test").coll.find().itcount());
}

jsTestLog("Test quiesce mode when shutting down a secondary");

jsTestLog("Create a cursor on the secondary.");
let res = assert.commandWorked(secondaryDB.runCommand({find: collName, batchSize: 2}));
assert.eq(2, res.cursor.firstBatch.length, res);
let cursorId = res.cursor.id;

jsTestLog("Create a hanging operation on the secondary.");
let findCmdFailPoint = configureFailPoint(secondary, "waitInFindBeforeMakingBatch");
let findCmd = startParallelShell(runFind, secondary.port);
findCmdFailPoint.wait();

jsTestLog("Create a hanging isMaster on the secondary.");
res = assert.commandWorked(secondary.adminCommand({isMaster: 1}));
assert(res.hasOwnProperty("topologyVersion"), res);
let topologyVersionField = res.topologyVersion;
let isMasterFailPoint = configureFailPoint(secondary, "waitForIsMasterResponse");
let isMaster =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionField), secondary.port);
isMasterFailPoint.wait();
assert.eq(1, secondary.getDB("admin").serverStatus().connections.awaitingTopologyChanges);

jsTestLog("Transition the secondary to quiesce mode.");
let quiesceModeFailPoint = configureFailPoint(secondary, "hangDuringQuiesceMode");
// We must skip validation due to the failpoint that hangs find commands.
replTest.stop(
    secondary, null /*signal*/, {skipValidation: true}, {forRestart: true, waitpid: false});
quiesceModeFailPoint.wait();

jsTestLog("The waiting isMaster returns a ShutdownInProgress error.");
isMaster();
// We cannot check the metrics because serverStatus returns ShutdownInProgress.
assert.commandFailedWithCode(secondaryDB.adminCommand({serverStatus: 1}),
                             ErrorCodes.ShutdownInProgress);

jsTestLog("New isMaster commands return a ShutdownInProgress error.");
res = assert.commandFailedWithCode(secondary.adminCommand({isMaster: 1}),
                                   ErrorCodes.ShutdownInProgress);
checkTopologyVersion(res, topologyVersionField);
checkRemainingQuiesceTime(res);

res = assert.commandFailedWithCode(secondary.adminCommand({
    isMaster: 1,
    topologyVersion: topologyVersionField,
    maxAwaitTimeMS: 99999999,
}),
                                   ErrorCodes.ShutdownInProgress);

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
let pauseWhileKillingOperationsFailPoint =
    configureFailPoint(secondary, "pauseWhileKillingOperationsAtShutdown");
quiesceModeFailPoint.off();
try {
    pauseWhileKillingOperationsFailPoint.wait();
} catch (e) {
    // This can throw if the waitForFailPoint command is killed by the shutdown.
}

jsTestLog("Operations fail with a shutdown error and append the topologyVersion.");
checkTopologyVersion(assert.commandFailedWithCode(secondaryDB.runCommand({find: collName}),
                                                  ErrorCodes.InterruptedAtShutdown),
                     topologyVersionField);

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
findCmdFailPoint = configureFailPoint(primary,
                                      "waitAfterCommandFinishesExecution",
                                      {ns: dbName + "." + collName, commands: ["find"]});
findCmd = startParallelShell(runFind, primary.port);
findCmdFailPoint.wait();

jsTestLog("Hang the primary in shutdown after stepdown.");
let postStepdownFailpoint = configureFailPoint(primary, "hangInShutdownAfterStepdown");
// We must skip validation due to the failpoint that hangs find commands.
replTest.stop(primary, null /*signal*/, {skipValidation: true}, {forRestart: true, waitpid: false});
postStepdownFailpoint.wait();

jsTestLog("Create a hanging isMaster on the primary.");
res = assert.commandWorked(primary.adminCommand({isMaster: 1}));
assert(res.hasOwnProperty("topologyVersion"), res);
topologyVersionField = res.topologyVersion;
isMasterFailPoint = configureFailPoint(primary, "waitForIsMasterResponse");
isMaster =
    startParallelShell(funWithArgs(runAwaitableIsMaster, topologyVersionField), primary.port);
isMasterFailPoint.wait();
assert.eq(1, primary.getDB("admin").serverStatus().connections.awaitingTopologyChanges);

jsTestLog("Transition the primary to quiesce mode.");
quiesceModeFailPoint = configureFailPoint(primary, "hangDuringQuiesceMode");
postStepdownFailpoint.off();
quiesceModeFailPoint.wait();

jsTestLog("The waiting isMaster returns a ShutdownInProgress error.");
isMaster();
// We cannot check the metrics because serverStatus returns ShutdownInProgress.
assert.commandFailedWithCode(primaryDB.adminCommand({serverStatus: 1}),
                             ErrorCodes.ShutdownInProgress);

jsTestLog("New isMaster commands return a ShutdownInProgress error.");
res = assert.commandFailedWithCode(primary.adminCommand({isMaster: 1}),
                                   ErrorCodes.ShutdownInProgress);
checkTopologyVersion(res, topologyVersionField);
checkRemainingQuiesceTime(res);

res = assert.commandFailedWithCode(primary.adminCommand({
    isMaster: 1,
    topologyVersion: topologyVersionField,
    maxAwaitTimeMS: 99999999,
}),
                                   ErrorCodes.ShutdownInProgress);

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
pauseWhileKillingOperationsFailPoint =
    configureFailPoint(primary, "pauseWhileKillingOperationsAtShutdown");
quiesceModeFailPoint.off();
try {
    pauseWhileKillingOperationsFailPoint.wait();
} catch (e) {
    // This can throw if the waitForFailPoint command is killed by the shutdown.
}

jsTestLog("Operations fail with a shutdown error and append the topologyVersion.");
checkTopologyVersion(assert.commandFailedWithCode(primaryDB.runCommand({find: collName}),
                                                  ErrorCodes.InterruptedAtShutdown),
                     topologyVersionField);

replTest.stopSet();
})();
