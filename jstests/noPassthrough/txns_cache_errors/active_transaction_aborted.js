/**
 * Validate that we are able to abort active transactions during periods of high cache
 * utilization.
 *
 * This test overloads the wiredtiger cache and then waits for the idle expiry thread and
 * cache-pressure thread to abort the blocked sessions, some of which will be active.
 *
 * @tags: [requires_persistence, requires_wiredtiger, requires_fcv_83]
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function applyCachePressure(host, threadId, stopLatch) {
    const mongo = new Mongo(host);

    let largeDoc = {a: 1, x: "a".repeat(0.5 * 1024 * 1024)};
    let sessions = [];

    try {
        while (stopLatch.getCount() > 0) {
            // Pin some cache by creating a large document in a txn we never end.
            let session = mongo.startSession();
            session.startTransaction();
            session.getDatabase("test").runCommand({"insert": "c", documents: [largeDoc]});
            sessions.push(session);
        }
    } finally {
        for (let i = 0; i < sessions.length; i++) {
            try {
                sessions[i].abortTransaction_forTesting();
                sessions[i].endSession();
            } catch (error) {
                // The transaction may already have been aborted, or the server may already be gone.
            }
        }
    }
    jsTestLog("Thread " + threadId + " completed with " + sessions.length + " sessions");
}

function testIdleAbort() {
    let replSet = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            // Shrink the cache to cause cache pressure sooner, and keep WiredTiger from giving up
            // on eviction while the cache is stuck.
            wiredTigerCacheSizeGB: 0.256,
            wiredTigerEngineConfigString: "cache_stuck_timeout_ms=600000",
            setParameter: {
                // Start with a long lifetime so the churn transactions below can accumulate
                // pinned dirty data. It is lowered once the cache is full.
                transactionLifetimeLimitSeconds: 24 * 60 * 60,
                // Disable cache-pressure aborts so only the idle aborter can kill transactions.
                cachePressureQueryPeriodMilliseconds: 0,
            },
        },
    });
    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();
    const db = primary.getDB("test");
    const adminDb = primary.getDB("admin");

    const stopLatch = new CountDownLatch(1);
    const churn = new Thread(applyCachePressure, primary.host, 0, stopLatch);
    churn.start();

    try {
        // Wait until eviction has run out of pages to evict: everything left in the cache is
        // pinned by the churn transactions, so their inserts can only park in the
        // eviction-assist loop.
        assert.soon(
            () => {
                return db.serverStatus().wiredTiger.cache["eviction empty score"] == 100;
            },
            "timed out waiting for the cache to fill up",
            240000,
        );

        // Lower the transaction lifetime. The churn transactions started before this point keep
        // pinning the cache, but every transaction started after it expires one second after it
        // begins. Their inserts park in the eviction-assist loop, and the idle aborter
        // interrupts them while they are parked.
        assert.commandWorked(
            adminDb.runCommand({setParameter: 1, transactionLifetimeLimitSeconds: 1}),
        );

        assert.soon(
            () => {
                return db.serverStatus().metrics.storage.cancelledCacheEvictions > 0;
            },
            "timed out waiting for the idle aborter to interrupt a parked transaction",
            240000,
        );
    } finally {
        stopLatch.countDown();
        try {
            churn.join();
        } catch (error) {
            jsTestLog("Ignoring non-critical error " + error);
        }
        replSet.stopSet();
    }
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
                CachePressureAbortSessionKillLimitPerBatch: 10,
            },
        },
    });
    replSet.startSet();
    replSet.initiate();
    let primary = replSet.getPrimary();

    // Make a session which we wait to be killed under cache pressure
    let oldestSession = primary.startSession();
    oldestSession.startTransaction();
    let res = oldestSession.getDatabase("test").runCommand({"insert": "c", documents: [{test: 1}]});

    // Spawn a bunch of threads to create cache pressure.
    const stopLatch = new CountDownLatch(1);
    let threads = [];
    for (let t = 0; t < kNumThreads; t++) {
        const thread = new Thread(applyCachePressure, primary.host, t, stopLatch);
        thread.start();
        threads.push(thread);
    }

    try {
        let iter = 1;
        while (res.ok) {
            res = oldestSession
                .getDatabase("test")
                .runCommand({"insert": "c", documents: [{test: 1}]});
            iter++;
        }
        jsTestLog(res);

        // Verify that the active oldest transaction is aborted.
        // There are two ways active oldest transactions are aborted: either the server-side
        // cache-pressure-abort or the WT-side cache-stuck check. For purpose of this test we will
        // allow either abort mechanism, since we only care that active transactions can be aborted
        // while under cache pressure, we don't mind who does it.
        let db = primary.getDB("test");
        let serverTriggered = db.serverStatus().metrics.storage.cancelledCacheEvictions > 0;
        let wtTriggered =
            res.code == ErrorCodes.WriteConflict &&
            res.errmsg.includes("-31800: Transaction has the oldest pinned transaction ID");

        jsTestLog("Oldest transaction aborted", {inserts: iter, serverTriggered, wtTriggered});
        assert(serverTriggered || wtTriggered);

        oldestSession.endSession();
    } finally {
        stopLatch.countDown();
        for (let t = 0; t < threads.length; t++) {
            try {
                threads[t].join();
            } catch (error) {
                jsTestLog("Ignoring non-critical error " + error);
            }
        }
        replSet.stopSet();
    }
}

testIdleAbort();
testCachePressureAbort();
