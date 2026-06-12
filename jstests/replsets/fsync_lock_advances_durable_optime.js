/**
 * Tests that fsyncLock acquisition advances the in-memory durableOpTime to lastWritten on
 * the primary, so that snapshot reads against a fsync-locked primary do not hang on oplog
 * entries that were applied but not yet journal-fsynced at lock acquisition time.
 *
 * The acquisition path handles this by calling flushAllFiles with callerHoldsReadLock=true,
 * which routes the JournalListener through UseJournalListener::kUpdateUnderReadLock. That
 * calls getToken with TokenMode::kReadLockHeld, which produces a token based on lastWritten
 * while skipping the oplog truncate-after-point write (that write requires Global IX and
 * would block behind the Global S we hold). onDurable then advances durableOpTime in-memory.
 * JournalFlusher's next cycle picks up the deferred truncate-after-point write after the
 * lock is released.
 *
 * JournalFlusher cannot substitute for this: its getToken call needs IX to write the
 * truncate-after-point, and that IX acquisition blocks behind Global S for the entire lock
 * duration.
 *
 * The test pauses JournalFlusher to keep the "applied but not durable" window open, does
 * one or more w:1 / j:false inserts until an oplog entry lands in that window, takes
 * fsyncLock, and then issues a snapshot read whose afterClusterTime is at the in-flight
 * entry. The read must return promptly.
 *
 * Intentionally no maxTimeMS on the snapshot read: a failure should manifest as a clear
 * test timeout rather than a flaky time-limit error.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_fsync,
 *   requires_replication,
 *   requires_persistence,
 *   requires_majority_read_concern,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("fsyncLock advances durableOpTime to lastWritten", function () {
    let rst, primary, adminDB, testDB;

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        adminDB = primary.getDB("admin");
        testDB = primary.getDB(jsTestName());
    });

    after(function () {
        rst.stopSet();
    });

    it("snapshot read with afterClusterTime at in-flight entry returns while fsyncLock is held", function () {
        // Pause JournalFlusher so writes stay "applied but not yet durable". The failpoint
        // takes effect after any in-flight flush cycle finishes. The flusher may complete one
        // more cycle after the failpoint is set and consume our first write, so we keep
        // inserting (w:1, j:false) until durableOpTime < writtenOpTime, confirming the gap is
        // open and the flusher is parked. Without this confirmation the test would be vacuous:
        // if the flusher caught up before we took fsyncLock, there would be nothing to fix.
        assert.commandWorked(
            adminDB.runCommand({configureFailPoint: "pauseJournalFlusherThread", mode: "alwaysOn"}),
        );

        let writeOpTime;
        assert.soon(
            () => {
                const insertRes = testDB.runCommand({
                    insert: "coll",
                    documents: [{x: 1}],
                    writeConcern: {w: 1, j: false},
                });
                if (!insertRes.ok) return false;
                writeOpTime = insertRes.operationTime;
                const stRes = adminDB.runCommand({replSetGetStatus: 1});
                if (!stRes.ok) return false;
                return bsonWoCompare(stRes.optimes.durableOpTime, stRes.optimes.writtenOpTime) < 0;
            },
            "durableOpTime did not fall behind writtenOpTime after pausing JournalFlusher",
            10000,
        );
        jsTest.log.info("Inserted at operationTime", {writeOpTime});

        // Take fsyncLock. Inside FSyncLockThread::run(), flushAllFiles fsyncs the journal (so
        // the new entry is now durable on disk) and the acquisition path advances the in-memory
        // durableOpTime to lastWritten before this command returns.
        assert.commandWorked(adminDB.runCommand({fsync: 1, lock: true}));

        const status = assert.commandWorked(adminDB.runCommand({replSetGetStatus: 1}));
        jsTest.log.info("optimes after fsyncLock", {
            writtenOpTime: status.optimes.writtenOpTime,
            appliedOpTime: status.optimes.appliedOpTime,
            durableOpTime: status.optimes.durableOpTime,
            lastCommittedOpTime: status.optimes.lastCommittedOpTime,
        });
        assert(
            bsonWoCompare(status.optimes.durableOpTime.ts, writeOpTime) >= 0,
            "durableOpTime must be >= writeOpTime after fsyncLock",
            {durableOpTime: status.optimes.durableOpTime, writeOpTime},
        );
        assert(
            bsonWoCompare(status.optimes.lastCommittedOpTime.ts, writeOpTime) >= 0,
            "lastCommittedOpTime must be >= writeOpTime after fsyncLock",
            {lastCommittedOpTime: status.optimes.lastCommittedOpTime, writeOpTime},
        );

        // A snapshot read whose afterClusterTime is at the in-flight entry must return while
        // fsyncLock is held. This succeeds because lastCommittedOpTime caught up to lastWritten
        // during fsyncLock acquisition. Without the fix, the read would hang in a single-node
        // replica set waiting for a majority commit that cannot advance while Global S is held.
        jsTest.log.info("Issuing snapshot find", {afterClusterTime: writeOpTime});
        const start = Date.now();
        const findRes = testDB.runCommand({
            find: "coll",
            readConcern: {level: "snapshot", afterClusterTime: writeOpTime},
        });
        jsTest.log.info("Snapshot find returned", {durationMs: Date.now() - start});
        assert.commandWorked(findRes);

        assert.commandWorked(adminDB.runCommand({fsyncUnlock: 1}));
        assert.commandWorked(
            adminDB.runCommand({configureFailPoint: "pauseJournalFlusherThread", mode: "off"}),
        );
    });
});
