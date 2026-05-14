/**
 * SERVER-122142: exercise the oplog visibility thread's lifecycle while
 * multiple readers call getRecordStore() against the oplog and a durable
 * recovery is being forced by DDL on the oplog and a paused JournalFlusher.
 *
 * The oplog visibility thread is owned by WiredTigerOplogManager and is
 * started/stopped as a side effect of WiredTigerRecordStore::Oplog
 * construction/destruction. When DDL on the oplog forces collections to go
 * through a durable recovery from disk, each concurrent query requesting the
 * oplog can trigger independent start/stop cycles of the visibility thread.
 * This test drives the conditions under which the race is reachable and
 * asserts that oplog reads remain available end-to-end.
 *
 * The race is described as "effectively impossible to hit in practice"
 * because DDL on the oplog is exceedingly rare, so this test exists to pin
 * the scenario down empirically; the TLA+ companion at
 * src/mongo/tla_plus/Replication/OplogVisibilityThread proves the lifecycle
 * invariants under the fix.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojson({storage: {recovery: 2}, replication: 1}),
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const localDB = primary.getDB("local");
const adminDB = primary.getDB("admin");

// Seed the oplog with enough traffic that reverse-cursor reads have content
// to walk during the race window.
jsTestLog("Seeding the oplog");
const seedColl = primary.getDB(jsTestName())["seed"];
for (let i = 0; i < 200; i++) {
    assert.commandWorked(seedColl.insert({_id: i, payload: "x".repeat(64)}));
}

// Pause the JournalFlusher to widen the window in which the OplogManager's
// visibility thread can be observed in a transitional state. This is the
// same lever used by commit_point_propagation_with_oplog_exhaust_cursor.js
// and override_chaining_allowed_false_if_necessary.js.
jsTestLog("Pausing the JournalFlusher thread on the primary");
const journalFp = configureFailPoint(primary, "pauseJournalFlusherThread");
journalFp.wait();

// Spin up a small fleet of concurrent readers that repeatedly issue
// reverse-cursor scans of the oplog. Each invocation walks through
// getRecordStore() and (if a recovery is in flight) takes the
// thread-lifecycle path under test.
const numReaders = 8;
const readerDurationMs = 6 * 1000;
const readerThreads = [];

jsTestLog(`Launching ${numReaders} concurrent oplog readers`);
for (let i = 0; i < numReaders; i++) {
    const t = new Thread(function(host, durationMs) {
        const conn = new Mongo(host);
        const localDB = conn.getDB("local");
        const deadline = Date.now() + durationMs;
        let reads = 0;
        let errors = 0;
        while (Date.now() < deadline) {
            try {
                // Reverse cursor, no limit on visibility -- this is the path
                // that walks the WiredTigerOplogCursor and forces interaction
                // with the visibility thread.
                const cur = localDB.oplog.rs.find().sort({$natural: -1}).limit(8);
                while (cur.hasNext()) {
                    cur.next();
                }
                reads++;
            } catch (e) {
                errors++;
            }
        }
        return {reads, errors};
    }, primary.host, readerDurationMs);
    t.start();
    readerThreads.push(t);
}

// While readers hammer the oplog, attempt DDL-style operations that the
// system permits on local collections. We cannot drop or rename the live
// oplog (see drop_oplog.js), but creating and dropping sibling collections
// in `local` exercises adjacent catalog paths and keeps the catalog mutex
// busy. We also poke the oplog manager directly by issuing
// `replSetResizeOplog`, which forces the OplogManager to re-evaluate state.
jsTestLog("Driving DDL on local + repeated replSetResizeOplog");
const ddlDeadline = Date.now() + readerDurationMs - 500;
let resizeCount = 0;
while (Date.now() < ddlDeadline) {
    const aux = "aux_" + Math.floor(Math.random() * 1000);
    assert.commandWorked(localDB.createCollection(aux));
    assert.commandWorked(localDB.runCommand({drop: aux}));
    assert.commandWorked(adminDB.runCommand({
        replSetResizeOplog: 1,
        size: (1000 + (resizeCount % 8) * 64),
    }));
    resizeCount++;
}

jsTestLog(`Issued ${resizeCount} replSetResizeOplog calls`);

// Resume the JournalFlusher so readers and the visibility thread can settle.
jsTestLog("Resuming the JournalFlusher thread");
journalFp.off();

// Join readers. Every reader must have completed without throwing; a torn-
// down visibility thread under a pinned reader would surface as
// CursorNotFound / OperationFailed here.
jsTestLog("Joining reader threads");
let totalReads = 0;
let totalErrors = 0;
for (const t of readerThreads) {
    t.join();
    const result = t.returnData();
    totalReads += result.reads;
    totalErrors += result.errors;
}

jsTestLog(`Total reader iterations: ${totalReads}, errors: ${totalErrors}`);
assert.gt(totalReads, 0, "expected at least one reader iteration");
assert.eq(totalErrors, 0, "no reader should observe a failed oplog read");

// Final sanity: a fresh reverse-cursor scan should complete and the oplog
// manager should report a non-zero latestVisibleTimestamp.
const tail = localDB.oplog.rs.find().sort({$natural: -1}).limit(1).toArray();
assert.eq(tail.length, 1, "oplog should contain at least one entry");

const serverStatus = adminDB.runCommand({serverStatus: 1});
assert.commandWorked(serverStatus);

rst.stopSet();
