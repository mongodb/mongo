/**
 * Tests that cache pressure doesn't prevent stepdown from running.
 *
 * @tags: [
 *      requires_persistence,
 *      requires_wiredtiger,
 * ]
 */

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = jsTestName();

// Shrink the WiredTiger cache so we can easily fill it up. Disable cache pressure monitors.
let replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        wiredTigerEngineConfigString:
            "cache_size=256M,cache_stuck_timeout_ms=600000,eviction=(threads_min=1,threads_max=1)",
        setParameter: {cachePressureQueryPeriodMilliseconds: 0, transactionLifetimeLimitSeconds: 600},
    },
});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const db = primary.getDB(dbName);

assert.commandWorked(db.createCollection(collName));

// Function to start many multi-document transactions to pin dirty content in the cache.
const doTxnsFn = function (dbName, collName) {
    // Create a large document to pin dirty data in WiredTiger.
    const largeDoc = {a: 1, x: "a".repeat(0.5 * 1024 * 1024)};

    const testDb = db.getMongo().getDB(dbName);

    let sessions = [];
    while (true) {
        let session = testDb.getMongo().startSession();
        try {
            session.startTransaction();
            assert.commandWorked(session.getDatabase(dbName).getCollection(collName).insert(largeDoc));
        } catch (e) {
            jsTestLog("Non-fatal exception: " + e);
        }

        // Wait until stepdown successfully cancels cache eviction on an active operation.
        let serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1}));
        if (serverStatus.metrics.storage.cancelledCacheEvictions > 0) {
            break;
        }

        sessions.push(session);
    }
};

let awaitTxns = startParallelShell(funWithArgs(doTxnsFn, dbName, collName), primary.port);

// Wait until eviction is unable to evict.
let evictionEmptyScore = 0;
while (evictionEmptyScore != 100) {
    let serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1}));
    evictionEmptyScore = serverStatus.wiredTiger.cache["eviction empty score"];
}

let serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1}));
assert.gt(serverStatus.transactions.currentInactive, 0);

// Wait for an active transaction that's stuck during cache pressure to be interrupted by stepdown.
assert.soon(() => {
    // Keep stepping down until we interrupt an active transaction. There's a small chance that we
    // might not have an active transaction in the system yet during cache pressure.
    assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 1, force: true}));

    let serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1}));
    let appInterrupts = serverStatus.wiredTiger.cache["application requested eviction interrupt"];
    if (appInterrupts > 0) {
        return true;
    }

    // Wait a bit longer.
    sleep(5000);
    return false;
});

awaitTxns();

serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1}));
assert.eq(0, serverStatus.transactions.currentActive);
assert.eq(0, serverStatus.transactions.currentInactive);
assert.gt(serverStatus.transactions.totalAborted, 0);
assert.gt(serverStatus.metrics.storage.cancelledCacheEvictions, 0);

replSet.stopSet();
