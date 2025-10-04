/*
 * Tests that we don't hit a 3 way deadlock when a step down thread waits for the RSTL in SECONDARY
 * state. This occurs when two stepdowns begin concurrently and both attempt to acquire the RSTL.
 *
 * This test creates a scenario where:
 * 1) Read thread acquires RSTL in MODE_IX and is blocked by a prepared txn (from secondary oplog
 *    application) due to a prepare conflict.
 * 2) Step down enqueues the RSTL in MODE_X and is blocked behind the read thread.
 * 3) Oplog applier is blocked trying to apply a 'commitTransaction' oplog entry. The commit is
 *    attempting to acquire the RSTL lock in MODE_IX but is blocked behind the step down thread.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();
const secondary = rst.getSecondary();

TestData.dbName = dbName;
TestData.collName = collName;
TestData.collNss = collNss;

jsTestLog("Do a document write");
assert.commandWorked(primaryColl.insert({_id: 0}, {"writeConcern": {"w": "majority"}}));
rst.awaitReplication();

jsTestLog("Hang primary on step down");
const joinStepDownThread = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({configureFailPoint: "stepdownHangBeforeRSTLEnqueue", mode: "alwaysOn"}));

    const freezeSecs = 24 * 60 * 60; // 24 hours
    assert.commandFailedWithCode(
        db.adminCommand({"replSetStepDown": freezeSecs, "force": true}),
        ErrorCodes.NotWritablePrimary,
    );
}, primary.port);

waitForCurOpByFailPointNoNS(primaryDB, "stepdownHangBeforeRSTLEnqueue");

jsTestLog("Force reconfig to swap the electable node");
// We must specify the node in getReplSetConfigFromNode, because we do not have a writable primary.
const newConfig = rst.getReplSetConfigFromNode(0);
const oldPrimaryId = rst.getNodeId(primary);
const newPrimaryId = rst.getNodeId(secondary);
newConfig.members[newPrimaryId].priority = 1;
newConfig.members[oldPrimaryId].priority = 0;
newConfig.version++;
assert.commandWorked(secondary.adminCommand({"replSetReconfig": newConfig, force: true}));

jsTestLog("Step up the new electable node");
rst.stepUp(secondary);

jsTestLog("Wait for step up to complete");
// Wait until the primary successfully steps down via heartbeat reconfig.
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
rst.awaitSecondaryNodes(null, [primary]);
const newPrimary = rst.getPrimary();

// Clear the logs so that when we check for the stepdown's "Starting to kill user operations" log
// below on the old primary, we won't be counting the one from stepping up the secondary (right
// above this).
clearRawMongoProgramOutput();

jsTestLog("Prepare a transaction on the new primary");
const session = newPrimary.startSession();
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];
session.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.update({_id: 0}, {$set: {"b": 1}}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

jsTestLog("Get a cluster time for afterClusterTime reads");
TestData.clusterTimeAfterPrepare = assert.commandWorked(
    newPrimary.getDB(dbName)[collName].runCommand("insert", {documents: [{_id: "clusterTimeAfterPrepare"}]}),
).operationTime;

// Make sure the insert gets replicated to the old primary (current secondary) so that its
// clusterTime advances before we try to do an afterClusterTime read at the time of the insert.
rst.awaitReplication();

jsTestLog("Do a read that hits a prepare conflict on the old primary");
const wTPrintPrepareConflictLogFailPoint = configureFailPoint(primary, "WTPrintPrepareConflictLog");

const joinReadThread = startParallelShell(() => {
    db.getMongo().setSecondaryOk();
    let oldPrimaryDB = db.getSiblingDB(TestData.dbName);

    assert.commandFailedWithCode(
        oldPrimaryDB.runCommand({
            find: TestData.collName,
            filter: {_id: 0},
            readConcern: {level: "local", afterClusterTime: TestData.clusterTimeAfterPrepare},
        }),
        ErrorCodes.InterruptedDueToReplStateChange,
    );
}, primary.port);

jsTestLog("Wait to hit a prepare conflict");
wTPrintPrepareConflictLogFailPoint.wait();

jsTestLog("Allow step down to complete");
assert.commandWorked(primary.adminCommand({configureFailPoint: "stepdownHangBeforeRSTLEnqueue", mode: "off"}));

jsTestLog("Wait for step down to start killing operations");
checkLog.contains(primary, "Starting to kill user operations");

jsTestLog("Commit the prepared transaction");
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

jsTestLog("Join parallel shells");
joinStepDownThread();
joinReadThread();

const replMetrics = assert.commandWorked(primary.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(replMetrics.stateTransition.lastStateTransition, "stepDown");

jsTestLog("Check nodes have correct data");
assert.docEq([{_id: 0, b: 1}], newPrimary.getDB(dbName)[collName].find({_id: 0}).toArray());
rst.awaitReplication();
assert.docEq([{_id: 0, b: 1}], primary.getDB(dbName)[collName].find({_id: 0}).toArray());

rst.stopSet();
