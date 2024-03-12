/**
 * Tests exhaustion of read tickets, trying to force a deadlock reproduction
 * after yield restores lock state. See SERVER-75205 for more information.
 *
 * Deadlock:
 * The deadlock in question involves readers acquiring the RSTL lock, which no longer
 * happens in 5.0+ due to lock-free reads. Therefore, this test doesn't exercise the
 * deadlock behavior on 5.0+. 4.4 is the primary target of this test, but there isn't
 * much test coverage of ticket exhaustion, so this test may provide other benefits on
 * 5.0+.
 *
 * Parallel Shell Coordination:
 * The 'jsTestName().timing_coordination' collection is used to coordinate timing
 * between the main thread of the test and all the readers via writes to specific
 * documents. One side will wait until the document appears.
 *
 * Test Steps:
 * 0) Start ReplSet with special params:
 *     - lower read ticket concurrency
 *     - increase yielding
 * 1) Insert kNumDocs documents.
 * 2) Kick off many parallel readers that perform long collection scans that are subject to yields.
 * 3) Wait for many parallel read shells to run.
 * 4) Hold tickets before they're given back to the pool. This is what ensures that we're holding
 *    all of the read tickets before the stepdown so that one of the readers has to yield.
 * 5) Wait for many parallel readers to run.
 * 6) Ensure that there are no available read tickets.
 * 7) Hold stepDown so that we know that upon release, it will need a read ticket ~immediately.
 * 8) Initiate stepDown.
 * 9) Hold tickets again, verify state, unlock, and proceed with stepDown.
 *    <<Should deadlock here for this scenario>>
 * 10) Stop readers and clean up.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_fcv_80,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const kNumDocs = 1000;
const kNumReadTickets = 5;
const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // This test seeks the minimum amount of concurrency to force ticket exhaustion.
            storageEngineConcurrentReadTransactions: kNumReadTickets,
            // Make yielding more common.
            internalQueryExecYieldPeriodMS: 1,
            internalQueryExecYieldIterations: 1
        }
    }
});

const dbName = jsTestName();
const collName = "testcoll";
let nQueuedReaders = 20;
// Ensure that when read tickets are released, we have enough reads queued right behind to take the
// tickets again, ensuring that stepDown competes with reads and a read must yield.
assert.gte(nQueuedReaders, kNumReadTickets * 2);
TestData.dbName = dbName;
TestData.collName = collName;

// Issues long read commands until told to stop.
// Should be run in a parallel shell via startParallelShell() with a unique id.
function queuedLongReadsFunc(id) {
    jsTestLog("Starting queued reader [" + id + "]");
    db.getMongo().setSecondaryOk();
    db.getSiblingDB(TestData.dbName)
        .timing_coordination.insert({_id: id, msg: "queued reader started"});
    try {
        for (let i = 0;
             db.getSiblingDB(TestData.dbName).timing_coordination.findOne({_id: "stop reading"}) ==
             null;
             i++) {
            try {
                db.getSiblingDB(TestData.dbName)[TestData.collName].aggregate([{"$count": "x"}]);
            } catch (e) {
                jsTestLog("Ignoring failed read that may be due to stepdown [" + id + "]", e);
            }
        }
    } catch (e) {
        jsTestLog("Exiting reader [" + id + "] early due to:" + e);
    }
    jsTestLog("Queued Reader complete [" + id + "]");
}

/****************************************************/

// 0) Start ReplSet with special params:
//     - lower read ticket concurrency
//     - increase yielding
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let db = primary.getDB(dbName);
let primaryColl = db[collName];
let queuedReaders = [];

// 1) Insert kNumDocs documents.
jsTestLog("Fill collection [" + dbName + "." + collName + "] with " + kNumDocs + " docs");
for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(primaryColl.insert({"x": i}));
}
jsTestLog(kNumDocs + " inserts done");

// 2) Kick off many parallel readers that perform long collection scans that are subject to yields.
for (let i = 0; i < nQueuedReaders; i++) {
    queuedReaders.push(startParallelShell(funWithArgs(queuedLongReadsFunc, i), primary.port));
    jsTestLog("queued reader " + queuedReaders.length + " initiated");
}

// 3) Wait for many parallel read shells to run.
jsTestLog("Wait for many parallel reader shells to run");
assert.soon(() => db.getSiblingDB(TestData.dbName).timing_coordination.count({
    msg: "queued reader started"
}) >= nQueuedReaders,
            "Expected at least " + nQueuedReaders + " queued readers to start.");

// 4) Hold tickets before they're given back to the pool. This is what ensures that we're holding
//    all of the read tickets before the stepdown so that one of the readers has to yield.
jsTestLog("Hold tickets");
assert.commandWorked(db.adminCommand({configureFailPoint: 'hangTicketRelease', mode: 'alwaysOn'}));

// 5) Wait for many parallel readers to run.
jsTestLog("Wait for many parallel readers to run");
assert.soon(
    () => db.getSiblingDB("admin")
              .aggregate([{$currentOp: {}}, {$match: {"command.aggregate": TestData.collName}}])
              .toArray()
              .length >= kNumReadTickets,
    "Expected at least as many operations as queued readers.");

// 6) Ensure that there are no available read tickets.
jsTestLog("Wait for no available read tickets");
assert.soon(() => {
    let stats = db.runCommand({serverStatus: 1});
    jsTestLog(stats.admission.execution);
    return stats.admission.execution.read.available == 0;
}, "Expected to have no available read tickets.");

// 7) Hold stepDown so that we know that upon release, it will need a read ticket ~immediately.
let stats = assert.commandWorked(db.runCommand({serverStatus: 1}));
jsTestLog(stats.locks);
jsTestLog(stats.admission.execution);

stats = db.adminCommand({
    configureFailPoint: 'stepdownHangBeforeRSTLEnqueue',
    mode: 'off' /* Don't change the value; just get the counts. */
});
assert.eq(1, stats.ok);
assert.eq(0, stats.count);  // No process has entered this block yet.

jsTestLog("Hold stepDown");
assert.commandWorked(
    db.adminCommand({configureFailPoint: 'stepdownHangBeforeRSTLEnqueue', mode: 'alwaysOn'}));

// 8) Initiate stepDown.
jsTestLog("Make primary step down");
const stepDownSecs = 10;
const stepDownShell = startParallelShell(
    funWithArgs(function(stepDownSecs) {
        jsTestLog("Run replSetStepDown");
        assert.commandWorked(
            db.getSiblingDB(TestData.dbName).timing_coordination.createIndex({a: 1}));

        assert.commandWorked(
            db.getSiblingDB("admin").runCommand({"replSetStepDown": stepDownSecs, "force": true}));
    }, stepDownSecs), primary.port);

// Ensure that the parallel shell is up so that we know that the stepDown has begun. Since we
// can't share state between the main thread and the parallel shell, the "right" way to do this
// is to write something from within the parallel shell and check it here. However, even if we
// wrote something, we wouldn't be able to read it back because we locked reads! Therefore,
// we'll create an index. We can count the indexes via serverStatus without doing a "read," so
// it's observable. The index doesn't affect the correctness of this test.
assert.soon(() => {
    let stats = db.runCommand({serverStatus: 1});
    return stats.metrics.commands.createIndexes.total > 0;
}, "Expected stepDown shell to be ready.");

jsTestLog("Allow tickets to be released so that we can get stuck at stepdownHangBeforeRSTLEnqueue");
assert.commandWorked(db.adminCommand({configureFailPoint: 'hangTicketRelease', mode: 'off'}));

jsTestLog("Verify that we're stuck at stepdownHangBeforeRSTLEnqueue");
assert.commandWorked(db.adminCommand({
    waitForFailPoint: 'stepdownHangBeforeRSTLEnqueue',
    timesEntered: 1,
    maxTimeMS: 10 * 1000,
}));

// 9) Hold tickets again, verify state, unlock, and proceed with stepDown.
jsTestLog("Hold tickets again so that we can verify that there are competing readers");
assert.commandWorked(db.adminCommand({configureFailPoint: 'hangTicketRelease', mode: 'alwaysOn'}));
assert.soon(() => {
    let stats = db.runCommand({serverStatus: 1});
    jsTestLog(stats.admission.execution);
    return stats.admission.execution.read.available == 0;
}, "Expected to have no available read tickets.");

jsTestLog("Allow stepDown to proceed");
assert.commandWorked(
    db.adminCommand({configureFailPoint: 'stepdownHangBeforeRSTLEnqueue', mode: 'off'}));

jsTestLog("Allow tickets to be released");
assert.commandWorked(db.adminCommand({configureFailPoint: 'hangTicketRelease', mode: 'off'}));

jsTestLog("Wait for SECONDARY state");
replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

// Enforce the replSetStepDown timer.
sleep(stepDownSecs * 1000);

jsTestLog("Wait for PRIMARY state");
replTest.waitForState(primary, ReplSetTest.State.PRIMARY);
replTest.getPrimary();

// 10) Stop readers and clean up.
jsTestLog("Stop readers");
assert.commandWorked(db.getSiblingDB(dbName).timing_coordination.insertOne({_id: "stop reading"}));

for (let i = 0; i < queuedReaders.length; i++) {
    const awaitQueuedReader = queuedReaders[i];
    awaitQueuedReader();
    jsTestLog("Queued reader [" + i + "] done");
}

stepDownShell();

replTest.stopSet();
