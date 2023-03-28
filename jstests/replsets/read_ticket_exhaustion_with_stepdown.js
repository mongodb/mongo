/**
 * Tests exhaustion of read tickets, trying to force a deadlock reproduction
 * after yield restores lock state.
 *
 * The deadlock in question involves readers acquiring the RSTL lock, which no longer
 * happens in 5.0+ due to lock-free reads. Therefore, this test doesn't exercise the
 * deadlock behavior on 5.0+. 4.4 is the primary target of this test, but there isn't
 * much test coverage of ticket exhaustion, so this test may provide other benefits on
 * 5.0+.
 *
 * 0) Start ReplSet with special params:
 *     - lower read ticket concurrency
 *     - increase yielding
 * 1) Insert 100 documents.
 * 2) Kick off 30 parallel readers that perform long collection scans, subject to yields.
 * 3) Sleep with global X Lock (including RSTL), thus queuing up reads.
 * 4) Signal new readers that will be received after the global lock is released.
 * 5) Initiate step down while queue is working its way down to ensure there is a mix of
 *     enqueued readers from the global X lock and new readers initiated afterwards.
 * 6) Stop Readers.
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");

const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // This test seeks the minimum amount of concurrency to force ticket exhaustion
            // storageEngineConcurrencyAdjustmentAlgorithm: "",
            wiredTigerConcurrentReadTransactions: 5,
            // Make yielding more common
            internalQueryExecYieldPeriodMS: 1,
            internalQueryExecYieldIterations: 1
        }
    }
});

const dbName = jsTestName();
const collName = "testcoll";
let nQueuedReaders = 20;
let nNewReaders = 10;
TestData.dbName = dbName;
TestData.collName = collName;

function queuedLongReadsFunc(id) {
    jsTestLog("Starting Queued Reader [" + id + "]");
    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase(TestData.dbName);
    const sessionColl = sessionDb[TestData.collName];

    try {
        for (let i = 0;
             db.getSiblingDB(TestData.dbName).timing_coordination.findOne({_id: "stop reading"}) ==
             null;
             i++) {
            jsTestLog("queuedLongReadsFunc on " + TestData.dbName + "." + TestData.collName +
                      " read (" + i + ") beg. Reader id:" + id);
            try {
                db.getSiblingDB(TestData.dbName)[TestData.collName].aggregate([{"$count": "x"}]);
            } catch (e) {
                jsTestLog("ignoring failed read, possible due to stepdown", e);
            }
            jsTestLog("queuedLongReadsFunc read (" + i + ") end. Reader id:" + id);
        }
    } catch (e) {
        jsTestLog("Exiting reader [" + id + "] early due to:" + e);
    }
    jsTestLog("Queued Reader complete [" + id + "]");

    // Validate that the connection is not closed on step down.
    assert.commandWorked(db.adminCommand({ping: 1}));
}

function newLongReadsFunc(id) {
    jsTestLog("Starting New Reader [" + id + "]");
    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase(TestData.dbName);
    const sessionColl = sessionDb[TestData.collName];

    // Coordinate all readers to begin at the same time
    assert.soon(() => db.getSiblingDB(TestData.dbName).timing_coordination.findOne({
        _id: "begin new readers"
    }) !== null,
                "Expected main test thread to insert a document.");

    try {
        for (let i = 0;
             db.getSiblingDB(TestData.dbName).timing_coordination.findOne({_id: "stop reading"}) ==
             null;
             i++) {
            jsTestLog("newLongReadsFunc on " + TestData.dbName + "." + TestData.collName +
                      " read (" + i + ") beg. Reader id:" + id);
            try {
                db.getSiblingDB(TestData.dbName)[TestData.collName].aggregate([{"$count": "x"}]);
            } catch (e) {
                jsTestLog("ignoring failed read, possible due to stepdown", e);
            }
            jsTestLog("newLongReadsFunc read (" + i + ") end. Reader id:" + id);
        }
    } catch (e) {
        jsTestLog("Exiting reader [" + id + "] early due to:" + e);
    }
    jsTestLog("New Reader complete [" + id + "]");

    // Validate that the connection is not closed on step down.
    assert.commandWorked(db.adminCommand({ping: 1}));
}

function runStepDown() {
    jsTestLog("Making primary step down.");
    let stats = db.runCommand({serverStatus: 1});
    jsTestLog(stats.locks);
    jsTestLog(stats.wiredTiger.concurrentTransactions);
    assert.commandWorked(primaryAdmin.runCommand({"replSetStepDown": 20, "force": true}));

    // Wait until the primary transitioned to SECONDARY state.
    replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

    sleep(5000);

    jsTestLog("Making old primary eligible to be re-elected.");
    assert.commandWorked(primaryAdmin.runCommand({replSetFreeze: 0}));
    replTest.waitForState(primary, ReplSetTest.State.PRIMARY);
    replTest.getPrimary();
}

/****************************************************/

// 0) Start ReplSet with special params:
//     - lower read ticket concurrency
//     - increase yielding
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let db = primary.getDB(dbName);
let primaryAdmin = primary.getDB("admin");
let primaryColl = db[collName];
let queuedReaders = [];
let newReaders = [];

// 1) Insert 100 documents.
jsTestLog("Fill collection [" + dbName + "." + collName + "] with 100 docs");
for (let i = 0; i < 100; i++) {
    assert.commandWorked(primaryColl.insert({"x": i}));
}
jsTestLog("100 inserts done");

// 2) Kick off 30 parallel readers that perform long collection scans, subject to yields.
for (let i = 0; i < nQueuedReaders; i++) {
    queuedReaders.push(startParallelShell(funWithArgs(queuedLongReadsFunc, i), primary.port));
    jsTestLog("queued reader " + queuedReaders.length + " initiated");
}

for (let i = 0; i < newReaders; i++) {
    newReaders.push(startParallelShell(funWithArgs(newLongReadsFunc, i), primary.port));
    jsTestLog("new reader " + newReaders.length + " initiated");
}

// 3) Sleep with global X Lock (including RSTL), thus queuing up reads.
let ns = dbName + "." + collName;
jsTestLog("Sleeping with Global X Lock on " + ns);
db.adminCommand({
    sleep: 1,
    secs: 5,
    lock: "w",  // MODE_X lock.
    $comment: "Global lock sleep"
});
jsTestLog("Done sleeping with Global X Lock on " + ns);

// 4) Signal new readers that will be received after the global lock is released.
db.getSiblingDB(dbName).timing_coordination.insertOne({_id: "begin new readers"});

// 5) Initiate step down while queue is working its way down to ensure there is a mix of
//     enqueued readers from the global X lock and new readers initiated afterwards.
assert.soon(
    () => db.getSiblingDB("admin")
              .aggregate([{$currentOp: {}}, {$match: {"command.aggregate": TestData.collName}}])
              .toArray()
              .length > 5,
    "Expected more readers than read tickets.");
runStepDown();

// 6) Stop Readers.
jsTestLog("Stopping Readers");
db.getSiblingDB(dbName).timing_coordination.insertOne({_id: "stop reading"});

for (let i = 0; i < queuedReaders.length; i++) {
    const awaitQueuedReader = queuedReaders[i];
    awaitQueuedReader();
    jsTestLog("queued reader " + i + " done");
}
for (let i = 0; i < newReaders.length; i++) {
    const awaitNewReader = newReaders[i];
    awaitNewReader();
    jsTestLog("new reader " + i + " done");
}
queuedReaders = [];
newReaders = [];

replTest.stopSet();
})();
