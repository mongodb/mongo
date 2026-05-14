/**
 * Regression test for SERVER-126254: fsyncLock leaves durableOpTime stuck behind lastWritten.
 *
 * Companion to the TLA+ spec
 *     src/mongo/tla_plus/Replication/FsyncLockDurableOpTime/FsyncLockDurableOpTime.tla
 *
 * The spec models the wedge as a liveness counterexample: in the buggy code path, an oplog entry
 * that has committed in memory (w:1, j:false) but not yet been fsynced sits in the
 * committed-but-not-durable window when fsyncLock acquires Global S. With Global S held, the
 * JournalFlusher cannot advance durableOpTime, so any snapshot or majority read with
 * afterClusterTime past the wedged optime hangs indefinitely until fsyncUnlock. The fix decouples
 * the journal-flush advancement of durableOpTime from Global S acquisition.
 *
 * This jstest promotes the deterministic single-node repro attached to SERVER-126254 to a
 * regression. The repro is:
 *
 *     1. Pause the JournalFlusher (pauseJournalFlusherBeforeFlush failpoint).
 *     2. Issue a w:1, j:false insert. lastWritten advances; durableOpTime does not.
 *     3. Call fsyncLock. Global S is acquired with the entry still in the pending-durable window.
 *     4. Release the JournalFlusher failpoint. On the fix this is enough for durableOpTime to
 *        catch up to lastWritten. On the bug, durableOpTime stays wedged because the flusher's
 *        storage-side dependency cannot be satisfied while Global S is held.
 *     5. Assert durableOpTime catches up to lastWritten within a finite timeout. Without the fix,
 *        this assertion fails and the test fails fast rather than hanging the suite.
 *     6. fsyncUnlock. Cleanup.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     requires_fsync,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Single-node replica set: the bug is observable on a primary in isolation; secondaries are not
// required and would only widen the state space without adding signal.
const rst = new ReplSetTest({
    name: "fsync_lock_durable_optime_stuck",
    nodes: 1,
    nodeOptions: {
        // Disable background checkpoints so only the JournalFlusher can advance the durable
        // timestamp. Mirrors the configuration used by
        // jstests/replsets/majority_writes_wait_for_all_durable_timestamp.js.
        syncdelay: 0,
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = "fsync_lock_durable_optime_stuck";
const adminDB = primary.getDB("admin");
const testDB = primary.getDB(dbName);
const testColl = testDB[collName];

assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: 1}}));

// Helper. Returns the current durableOpTime / lastWrittenOpTime tuple from replSetGetStatus.
function getOpTimes() {
    const status = assert.commandWorked(adminDB.runCommand({replSetGetStatus: 1}));
    const self = status.members.find((m) => m.self === true);
    return {
        durable: self.optimeDurable || self.optime,
        written: self.optimeWritten || self.optime,
        appliedTs: status.optimes ? status.optimes.appliedOpTime : null,
        durableTs: status.optimes ? status.optimes.durableOpTime : null,
        lastWrittenTs: status.optimes ? status.optimes.lastWrittenOpTime : null,
    };
}

function tsLessThan(a, b) {
    if (a.t !== b.t) {
        return a.t < b.t;
    }
    return timestampCmp(a.ts, b.ts) < 0;
}

jsTest.log("Step 1: pause JournalFlusher so durableOpTime cannot advance.");
const journalFlusherFP = configureFailPoint(primary, "pauseJournalFlusherBeforeFlush");
journalFlusherFP.wait();

let lockedNode = null;
try {
    jsTest.log("Step 2: w:1, j:false insert. lastWritten advances; durableOpTime does not.");
    assert.commandWorked(testColl.insert({_id: 0, x: 1}, {writeConcern: {w: 1, j: false}}));

    const beforeLock = getOpTimes();
    jsTest.log("Pre-fsyncLock optimes: " + tojson(beforeLock));
    assert(
        beforeLock.lastWrittenTs && beforeLock.durableTs,
        "replSetGetStatus must surface lastWrittenOpTime and durableOpTime on this build",
    );
    assert(
        tsLessThan(beforeLock.durableTs, beforeLock.lastWrittenTs),
        "Pre-condition: durableOpTime must be strictly behind lastWrittenOpTime before fsyncLock. " +
            "If this assertion fails, the JournalFlusher pause failpoint is not effective. " +
            "beforeLock=" + tojson(beforeLock),
    );

    jsTest.log("Step 3: fsyncLock. Acquires Global S with one entry pending-durable.");
    const fsyncLockResult = assert.commandWorked(adminDB.runCommand({fsync: 1, lock: 1}));
    lockedNode = primary;
    jsTest.log("fsyncLock result: " + tojson(fsyncLockResult));

    jsTest.log("Step 4: release JournalFlusher pause. On the fix, durableOpTime will catch up.");
    journalFlusherFP.off();

    jsTest.log(
        "Step 5: assert durableOpTime catches up to lastWrittenOpTime within 30s. " +
            "Without the fix, durableOpTime stays wedged at the pre-lock value and this hangs.",
    );
    assert.soon(
        function () {
            const now = getOpTimes();
            if (!tsLessThan(now.durableTs, now.lastWrittenTs)) {
                jsTest.log("durableOpTime caught up: " + tojson(now));
                return true;
            }
            return false;
        },
        function () {
            return (
                "durableOpTime stayed wedged behind lastWrittenOpTime while fsyncLock held " +
                "Global S; SERVER-126254 regression. final=" + tojson(getOpTimes())
            );
        },
        /* timeout */ 30 * 1000,
        /* interval */ 250,
    );

    jsTest.log("Step 6: fsyncUnlock.");
    assert.commandWorked(adminDB.runCommand({fsyncUnlock: 1}));
    lockedNode = null;
} finally {
    // Defensive cleanup. If any assertion above failed, we still need to release the journal
    // flusher and the fsync lock so ReplSetTest.stopSet does not hang.
    if (journalFlusherFP) {
        try {
            journalFlusherFP.off();
        } catch (e) {
            jsTest.log("journalFlusherFP.off() raised during cleanup: " + tojson(e));
        }
    }
    if (lockedNode !== null) {
        try {
            assert.commandWorked(lockedNode.getDB("admin").runCommand({fsyncUnlock: 1}));
        } catch (e) {
            jsTest.log("fsyncUnlock raised during cleanup: " + tojson(e));
        }
    }
}

rst.stopSet();
