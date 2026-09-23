/**
 * Validate we are aborting multi-document transactions when we are under cache pressure.
 *
 * @tags: [requires_persistence, requires_wiredtiger, requires_fcv_83]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";

// Shrink the WiredTiger cache so we can easily fill it up
let replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {wiredTigerEngineConfigString: "cache_size=16M,cache_stuck_timeout_ms=600000"},
});

replSet.startSet();
replSet.initiate();
const db = replSet.getPrimary().getDB("test");

// Ensure the thread is turned on. Run it far more often than the default period: the drain loop
// below only lasts a few hundred milliseconds, so at the default period the thread would not even
// complete one cycle while a client abort is in flight.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        cachePressureQueryPeriodMilliseconds: 20,
    }),
);

// Reduce the tracking window so we can detect cache pressure quicker.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        cachePressureEvictionStallDetectionWindowSeconds: 1,
    }),
);

// A single-threaded workload cannot keep one application thread blocked on eviction for 95% of
// every sampling interval, which is what the default proportion demands, so the eviction stall
// signal never fires. Lower the bar to something this workload can actually reach.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        cachePressureEvictionStallThresholdProportion: 0.3,
    }),
);

// Abort one transaction per pass rather than a whole batch. This keeps the background thread
// working through the sessions for longer, which improves the odds of it aborting a transaction
// while the client's own abort is in flight.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        CachePressureAbortSessionKillLimitPerBatch: 1,
    }),
);

// Prevent the idle-timeout from doing anything, so that every abort observed below can only have
// come from cache pressure.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        transactionLifetimeLimitSeconds: 24 * 60 * 60,
    }),
);

// Log cache pressure metrics.
function logCacheStatus() {
    const status = db.serverStatus();
    jsTestLog(
        `Cache pressue wait time threshold exceeded: ${status.metrics.cachePressure.waitTimeThresholdExceeded}`,
    );
    jsTestLog(
        `Cache pressue cache updates threshold exceeded: ${status.metrics.cachePressure.cacheUpdatesThreshold}`,
    );
    jsTestLog(
        `Cache pressue cache dirty threshold exceeded: ${status.metrics.cachePressure.cacheDirtyThreshold}`,
    );
}

// Create a large document to pin dirty data in WiredTiger.
let largeDoc = {a: 1, x: "a".repeat(0.5 * 1024 * 1024)};

assert.commandWorked(db.createCollection("c"));

let sessions = [];
let firstPreparedTxn = true;

// The drain loop below has to catch the background thread aborting a transaction while a client
// abort is in flight, which is a per-session chance. Keep filling past the first kill so that
// there are enough sessions left to give it several chances rather than one or two.
const minSessions = 20;

jsTestLog("Starting large inserts to create cache pressure...");

let successfulKills = 0;
const timeoutTimestamp = Date.now() + 10 * 60 * 1000; // 10 minutes timeout

// Insert multiple large documents without committing.
while (true) {
    if (Date.now() > timeoutTimestamp) {
        jsTestLog("Timeout reached, stopping large inserts...");
        break;
    }

    let session = db.getMongo().startSession();
    session.startTransaction();
    try {
        assert.commandWorked(
            session.getDatabase("test").runCommand({"insert": "c", documents: [largeDoc]}),
        );
        if (firstPreparedTxn) {
            firstPreparedTxn = false;
            PrepareHelpers.prepareTransaction(session);
        }
    } catch (e) {}
    sessions.push(session);

    jsTestLog(`Large inserts completed: ${sessions.length}`);
    logCacheStatus();

    let status = db.serverStatus();

    successfulKills = db.serverStatus().metrics.rollbackUnderCachePressure.successfulKills;

    // Once we have a successful kill, we know we have aborted the oldest transaction.
    if (successfulKills > 0 && sessions.length >= minSessions) {
        jsTestLog("Oldest transaction successfully aborted under cache pressure.");
        logCacheStatus();
        break;
    }
}

// Check we did not abort the prepared transaction.
let res = assert.commandWorked(db.adminCommand({serverStatus: 1}));
assert.eq(
    res.transactions.totalPreparedThenAborted,
    0,
    "Prepared transaction was aborted unexpectedly",
);

// Runs in a child thread. Keeps one transaction holding dirty, uncommitted data so cache pressure
// persists through the drain loop, aborting and restarting it when cache pressure kills it. Trips
// the ready latch once it holds dirty data, and exits when the parent trips the stop latch.
function fillerWork(host, readyLatch, stopLatch) {
    const connection = new Mongo(host);
    const session = connection.startSession();
    const doc = {a: 1, x: "a".repeat(0.5 * 1024 * 1024)};
    let ready = false;
    try {
        while (stopLatch.getCount() > 0) {
            session.startTransaction();
            let shouldRestart = false;
            while (!shouldRestart && stopLatch.getCount() > 0) {
                try {
                    assert.commandWorked(
                        session.getDatabase("test").runCommand({"insert": "c", documents: [doc]}),
                    );
                    // First dirty insert: the drain can now rely on live cache pressure.
                    if (!ready) {
                        readyLatch.countDown();
                        ready = true;
                    }
                } catch (e) {
                    shouldRestart = true;
                }
            }
            // A failed insert may leave the transaction in progress; abort it so the next
            // startTransaction() is not rejected as already in progress.
            if (shouldRestart) {
                try {
                    session.abortTransaction_forTesting();
                } catch (e) {
                    // Already aborted (e.g. by cache pressure); ignore.
                }
            }
        }
    } finally {
        session.endSession();
    }
}

jsTestLog("Aborting remaining transactions...");

// Keep a filler thread inserting through the drain loop. Without live writes, cache pressure decays
// below the stall proportion before the drain finishes and eviction stops aborting. Both signals are
// latches (shared across Thread serialization), which involve no DB write and so are unaffected by
// the TemporarilyUnavailable state this test forces.
const fillerReady = new CountDownLatch(1);
const fillerStop = new CountDownLatch(1);
const fillerThread = new Thread(fillerWork, db.getMongo().host, fillerReady, fillerStop);
fillerThread.start();

// Thread.start() returns before the child connects, so wait until the filler is running, then confirm
// the server still reports cache pressure before draining. The wait-time threshold is what the
// background abort thread watches; without live writes the wait-time delta drops below it, so this
// ensures pressure is sustained rather than assumed from a single filler insert.
fillerReady.await();
assert.soon(
    () => {
        const cp = db.serverStatus().metrics.cachePressure;
        return cp.waitTimeThresholdExceeded && (cp.cacheUpdatesThreshold || cp.cacheDirtyThreshold);
    },
    "Timed out waiting for cache pressure before draining.",
    2 * 60 * 1000,
);

try {
    // Abort remaining transactions while handling temporarily unavailable errors
    let numTemporarilyUnavailable = 0;
    for (let i = 0; i < sessions.length; i++) {
        let res = sessions[i].abortTransaction_forTesting();
        if (res.ok == 0 && res.code == ErrorCodes.TemporarilyUnavailable) {
            numTemporarilyUnavailable++;
        }
        // End the session now, while the server is still up, instead of leaving it for the shell's
        // GC finalizer to clean up after stopSet() has already killed the server. Otherwise, every
        // leaked session logs a failed endSessions attempt once per second until the resmoke hang
        // analyzer aborts the shell.
        sessions[i].endSession();
    }

    // At least one of the transactions should return with the temporarily unavailable error code.
    assert(
        numTemporarilyUnavailable > 0,
        "Expected TemporarilyUnavailable error but none occurred.",
    );
    jsTestLog("All transactions aborted as expected.");
} finally {
    // Stop the filler and wait for it to release its session before teardown, unconditionally, so
    // a thrown abort above cannot leave it running with an active transaction.
    fillerStop.countDown();
    try {
        fillerThread.join();
    } catch (error) {
        jsTestLog("Ignoring non-critical error " + error);
    }
    replSet.stopSet();
}
