/**
 * Validate that we are able to abort active transactions during periods of high cache
 * utilization.
 *
 * This test overloads the wiredtiger cache and then waits for the idle expiry thread and
 * cache-pressure thread to abort the blocked sessions, some of which will be active.
 *
 * @tags: [requires_persistence, requires_wiredtiger, featureFlagStorageEngineInterruptibility]
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function applyCachePressure(host, threadId) {
    const mongo = new Mongo(host);
    const db = mongo.getDB("test");

    let largeDoc = {a: 1, x: "a".repeat(0.5 * 1024 * 1024)};
    let sessions = [];

    while (db.serverStatus().metrics.storage.cancelledCacheEvictions <= 0) {
        // Pin some cache by creating a large document in a txn we never end.
        let session = mongo.startSession();
        session.startTransaction();
        session.getDatabase("test").runCommand({"insert": "c", documents: [largeDoc]});
        sessions.push(session);
    }

    for (let i = 0; i < sessions.length; i++) {
        sessions[i].abortTransaction_forTesting();
    }
    jsTestLog("Thread " + threadId + " completed with " + sessions.length + " sessions");
}

function testIdleAbort() {
    // Use a high number of threads to maximize the odds that one of them will be active.
    const kNumThreads = 50;

    let replSet = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            // Shrink the cache to cause cache pressure sooner.
            wiredTigerEngineConfigString: "cache_size=256M",
            setParameter: {
                // Make idle-timeout occur more for this test.
                transactionLifetimeLimitSeconds: 10,
                // Disable cache-pressure aborts so we can observer idle-aborts.
                cachePressureQueryPeriodMilliseconds: 0,
            }
        }
    });
    replSet.startSet();
    replSet.initiate();

    // Spawn a bunch of threads to create cache pressure.
    let threads = [];
    for (let t = 0; t < kNumThreads; t++) {
        const thread = new Thread(applyCachePressure, replSet.getPrimary().host, t);
        thread.start();
        threads.push(thread);
    }

    // Wait for an active txn to be killed.
    //
    // Here we count the number of sessions pulled out of eviction. This way of counting active
    // aborts is the most critical for preventing stalls, other ways of aborting active sessions
    // (e.g. by waiting for the thread to acknowledge its opctx is cancelled) are not as concerning.
    const db = replSet.getPrimary().getDB("test");
    assert.soon(() => {
        return db.serverStatus().metrics.storage.cancelledCacheEvictions > 0;
    }, "Timed out waiting for an active txn to be killed");

    for (let t = 0; t < threads.length; t++) {
        threads[t].join();
    }

    replSet.stopSet();
}

function testCachePressureAbort() {
    // We don't need many threads since we'll force a specific session to be active.
    const kNumThreads = 10;

    let replSet = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            // Shrink the cache to cause cache pressure sooner.
            wiredTigerEngineConfigString: "cache_size=256M",
            setParameter: {
                // Prevent the idle-timeout from doing anything.
                transactionLifetimeLimitSeconds: 24 * 60 * 60,
                // Shrink the stall window to speed up the test.
                cachePressureEvictionStallDetectionWindowSeconds: 10,
                // Kill a lot of things at once during cache pressure, this improves the odds of an
                // active-txn being killed.
                AbortOldestTransactionSessionKillLimitPerBatch: 10,
            }
        }
    });
    replSet.startSet();
    replSet.initiate();
    let primary = replSet.getPrimary();

    // Make a session which we wait to be killed under cache pressure
    let oldestSession = primary.startSession();
    oldestSession.startTransaction();
    let res = oldestSession.getDatabase("test").runCommand({"insert": "c", documents: [{test: 1}]});

    // Spawn a bunch of threads to create cache pressure.
    let threads = [];
    for (let t = 0; t < kNumThreads; t++) {
        const thread = new Thread(applyCachePressure, primary.host, t);
        thread.start();
        threads.push(thread);
    }

    let iter = 1;
    while (res.ok) {
        res = oldestSession.getDatabase("test").runCommand({"insert": "c", documents: [{test: 1}]});
        iter++;
    }
    jsTestLog(res);

    // Verify that the active oldest transaction is aborted.
    // There are two ways active oldest transactions are aborted: either the server-side
    // cache-pressure-abort or the WT-side cache-stuck check. For purpose of this test we will allow
    // either abort mechanism, since we only care that active transactions can be aborted while
    // under cache pressure, we don't mind who does it.
    let db = primary.getDB("test");
    let serverTriggered = db.serverStatus().metrics.storage.cancelledCacheEvictions > 0;
    let wtTriggered = (res.code == ErrorCodes.WriteConflict) &&
        res.errmsg.includes("-31800: Transaction has the oldest pinned transaction ID");

    jsTestLog("Oldest transaction aborted after " + iter +
              " inserts. ServerTriggered=" + serverTriggered + " wtTriggered=" + wtTriggered);
    assert(serverTriggered || wtTriggered);

    replSet.stopSet();

    for (let t = 0; t < threads.length; t++) {
        threads[t].join();
    }
}

testIdleAbort();
testCachePressureAbort();
