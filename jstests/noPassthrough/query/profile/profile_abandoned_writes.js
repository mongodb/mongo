/**
 * Test profiling is automatically disabled when there are too many LockTimeout errors per second.
 *
 * Tests that the system automatically stops profiling if there are too many LockTimeout errors per
 * second when acquiring the system.profile lock. To ensure reproducibility, the
 * 'forceLockTimeoutForProfiler' failpoint is used to simulate the LockTimeout condition. To keep
 * the test brief, we also lower the internalProfilingMaxAbandonedWritesPerSecondPerDb threshold.
 *
 * When the per-second threshold is exceeded, the profiler is disabled by setting the profiling
 * level to 0. A note message is logged to the system.profile collection. To re-enable profiling,
 * the level must be explicitly set back to 1 or 2.
 *
 * @tags: [
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("Profile Abandoned Writes", function () {
    let conn;
    let adminDB;
    const maxAbandonedWritesPerSecond = 5;

    before(function () {
        // Start mongod with low threshold for abandoned writes per second.
        conn = MongoRunner.runMongod({
            setParameter: {internalProfilingMaxAbandonedWritesPerSecondPerDb: maxAbandonedWritesPerSecond},
        });
        adminDB = conn.getDB("admin");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Helper function to get serverStatus profiler metrics.
    function getProfilerStats() {
        const serverStatus = adminDB.runCommand({serverStatus: 1});
        return serverStatus.profiler;
    }

    // Helper function to perform operations that trigger profiling.
    function performOperations(coll, count, comment) {
        for (let i = 0; i < count; i++) {
            assert.commandWorked(coll.insert({x: i, comment: comment + "-" + i}));
        }
    }

    function doSomeProblematicProfiling(coll, count, comment) {
        // Enable profiling.
        assert.commandWorked(coll.getDB().setProfilingLevel(2));

        // Enable the failpoint to force LockTimeout errors.
        const fp = configureFailPoint(adminDB, "forceLockTimeoutForProfiler");

        // Perform operations that will hit LockTimeout errors.
        // We need to exceed maxAbandonedWritesPerSecond to trigger profiling disablement.
        performOperations(coll, count, comment);

        // Disable the failpoint.
        fp.off();
    }

    function assertNoteDocExists(db) {
        // Check for a note entry in system.profile indicating profiling was abandoned.
        // Find the note document.
        const noteDoc = db.system.profile.findOne({note: {$exists: true}});
        assert.neq(noteDoc, null, "Expected to find a note document");

        // Verify structure.
        assert(noteDoc.hasOwnProperty("ts"), "Expected note document to have 'ts' field");
        assert(noteDoc.hasOwnProperty("note"), "Expected note document to have 'note' field");
        assert.eq(typeof noteDoc.note, "string", "Expected 'note' field to be a string");
        assert(
            noteDoc.hasOwnProperty("internalQueryGlobalProfilingLockDeadlineMs"),
            "Expected note document to have 'internalQueryGlobalProfilingLockDeadlineMs' field",
        );
        assert(
            noteDoc.hasOwnProperty("internalProfilingMaxAbandonedWritesPerSecondPerDb"),
            "Expected note document to have 'internalProfilingMaxAbandonedWritesPerSecondPerDb' field",
        );
    }

    it("disables profiling automatically after exceeding per-second threshold", function () {
        const testDB = conn.getDB(jsTestName() + "_autoDisableTest");
        assert.commandWorked(testDB.dropDatabase());
        const coll = testDB.getCollection("testColl");

        // Get initial serverStatus metrics.
        let stats = getProfilerStats();
        const initialAbandonedWrites = stats.totalAbandonedWrites;
        const initialDbsPastThreshold = stats.dbsPastThreshold;

        doSomeProblematicProfiling(coll, 2 * maxAbandonedWritesPerSecond, "autoDisableTest");

        // Verify that totalAbandonedWrites has increased.
        stats = getProfilerStats();
        const finalAbandonedWrites = stats.totalAbandonedWrites;
        assert.gte(
            finalAbandonedWrites - initialAbandonedWrites,
            maxAbandonedWritesPerSecond,
            `Expected totalAbandonedWrites to increase by at least the threshold. final: ${
                finalAbandonedWrites
            }, initial: ${initialAbandonedWrites}`,
        );

        // Verify that dbsPastThreshold has incremented.
        const finalDbsPastThreshold = stats.dbsPastThreshold;
        assert.eq(finalDbsPastThreshold, initialDbsPastThreshold + 1, "Expected dbsPastThreshold to increment by 1");

        assertNoteDocExists(testDB);

        // Verify profiling level is now 0 (disabled).
        const profilingStatus = assert.commandWorked(testDB.runCommand({profile: -1}));
        assert.eq(profilingStatus.was, 0, "Expected profiling level to be 0 after exceeding threshold");

        // Verify profiling is now disabled - additional operations should not be profiled.
        const countBefore = testDB.system.profile.count();
        performOperations(coll, 5, "autoDisableTest-after-disable");
        const countAfter = testDB.system.profile.count();
        assert.eq(countAfter, countBefore, "Expected no new profile entries after profiling was disabled");
    });

    it("can be re-enabled after automatic disable", function () {
        const testDB = conn.getDB(jsTestName() + "_manualReenableTest");
        assert.commandWorked(testDB.dropDatabase());
        const coll = testDB.getCollection("testColl");

        doSomeProblematicProfiling(coll, 2 * maxAbandonedWritesPerSecond, "manualReenableTest-disable");

        // Verify profiling is disabled (level should be 0).
        let profilingStatus = testDB.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 0, "Expected profiling level to be 0 after exceeding threshold");

        let profileDocs = testDB.system.profile.find({note: {$exists: true}}).toArray();
        assert.gt(profileDocs.length, 0, "Expected to find note about profiling being disabled");

        const countBefore = testDB.system.profile.count();
        performOperations(coll, 3, "manualReenableTest-verify-disabled");
        const countAfter = testDB.system.profile.count();
        assert.eq(countAfter, countBefore, "Expected profiling to be disabled");

        // Re-enable profiling by setting the level back to 2.
        assert.commandWorked(testDB.setProfilingLevel(2));
        jsTest.log.info("Re-enabled profiling by setting level back to 2");

        // Verify profiling level is back to 2.
        profilingStatus = assert.commandWorked(testDB.runCommand({profile: -1}));
        assert.eq(profilingStatus.was, 2, "Expected profiling level to be 2 after re-enabling");

        // Now profiling should work again for normal operations.
        const countBeforeReEnable = testDB.system.profile.count();
        performOperations(coll, 3, "manualReenableTest-after-reenable");
        const countAfterReEnable = testDB.system.profile.count();
        assert.gt(
            countAfterReEnable,
            countBeforeReEnable,
            "Expected profiling to be re-enabled after setting level back to 2",
        );
    });

    it("applies policy per database", function () {
        const firstHotDb = conn.getDB(jsTestName() + "_firstHotDb");
        const secondHotDb = conn.getDB(jsTestName() + "_secondHotDb");
        assert.commandWorked(firstHotDb.dropDatabase());
        assert.commandWorked(secondHotDb.dropDatabase());
        const firstHotColl = firstHotDb.getCollection("testColl");
        const secondHotColl = secondHotDb.getCollection("testColl");

        // Enable profiling on both databases.
        assert.commandWorked(firstHotDb.setProfilingLevel(2));
        assert.commandWorked(secondHotDb.setProfilingLevel(2));

        // Reset threshold to original value.
        assert.commandWorked(
            adminDB.runCommand({
                setParameter: 1,
                internalProfilingMaxAbandonedWritesPerSecondPerDb: maxAbandonedWritesPerSecond,
            }),
        );

        const stats1 = getProfilerStats();
        const initialDbsPastThreshold = stats1.dbsPastThreshold;

        doSomeProblematicProfiling(firstHotColl, 2 * maxAbandonedWritesPerSecond, "test3-db1-disable");

        // Verify profiling level is 0 for DB1.
        let profilingStatus = firstHotDb.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 0, "Expected profiling level to be 0 for DB1");
        assertNoteDocExists(firstHotDb);

        // Verify profiling still works for DB2.
        profilingStatus = secondHotDb.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 2, "Expected profiling level to still be 2 for DB2");

        const countBeforeDB2 = secondHotDb.system.profile.count();
        doSomeProblematicProfiling(secondHotColl, 3, "test3-db2-still-works");
        const countAfterDB2 = secondHotDb.system.profile.count();
        assert.gt(countAfterDB2, countBeforeDB2, "Expected profiling to still work in DB2");

        // Verify dbsPastThreshold incremented by 1 (only for DB1).
        const stats2 = getProfilerStats();
        assert.eq(stats2.dbsPastThreshold, initialDbsPastThreshold + 1, "Expected dbsPastThreshold to increment by 1");

        // Now disable profiling for DB2 as well.
        doSomeProblematicProfiling(secondHotColl, 2 * maxAbandonedWritesPerSecond, "test3-db2-disable");

        // Verify profiling level is 0 for DB2.
        profilingStatus = secondHotDb.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 0, "Expected profiling level to be 0 for DB2");
        assertNoteDocExists(secondHotDb);

        // Verify dbsPastThreshold incremented again (now for DB2).
        const stats3 = getProfilerStats();
        assert.eq(
            stats3.dbsPastThreshold,
            initialDbsPastThreshold + 2,
            "Expected dbsPastThreshold to increment by 2 total",
        );
    });

    it("can be disabled multiple times", function () {
        const testDB = conn.getDB(jsTestName() + "_test4");
        assert.commandWorked(testDB.dropDatabase());
        const coll = testDB.getCollection("testColl");

        // Reset threshold.
        assert.commandWorked(
            adminDB.runCommand({
                setParameter: 1,
                internalProfilingMaxAbandonedWritesPerSecondPerDb: maxAbandonedWritesPerSecond,
            }),
        );

        const stats1 = getProfilerStats();
        const initialDbsPastThreshold = stats1.dbsPastThreshold;

        doSomeProblematicProfiling(coll, 2 * maxAbandonedWritesPerSecond, "test4-first-disable");

        // Verify profiling level is 0.
        let profilingStatus = testDB.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 0, "Expected profiling level to be 0 after first disable");

        // Verify dbsPastThreshold incremented.
        const stats2 = getProfilerStats();
        assert.eq(stats2.dbsPastThreshold, initialDbsPastThreshold + 1, "Expected dbsPastThreshold to increment");

        // Re-enable profiling by setting the level back to 2.
        assert.commandWorked(testDB.setProfilingLevel(2));
        profilingStatus = testDB.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 2, "Expected profiling level to be 2 after re-enabling");

        // Verify profiling works again.
        const countBefore = testDB.system.profile.count({note: {$exists: false}});
        doSomeProblematicProfiling(coll, 3, "test4-verify-reenabled");
        const countAfter = testDB.system.profile.count({note: {$exists: false}});
        assert.gt(countAfter, countBefore, "Expected profiling to be re-enabled");

        // Now exceed the threshold again to disable profiling a second time.
        doSomeProblematicProfiling(coll, 2 * maxAbandonedWritesPerSecond, "test4-second-disable");

        // Verify profiling level is 0 again.
        profilingStatus = testDB.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 0, "Expected profiling level to be 0 after second disable");

        // Verify we can find a note about the second disable.
        const allNoteDocs = testDB.system.profile.find({note: {$exists: true}}).toArray();
        assert.gte(allNoteDocs.length, 2, "Expected at least 2 note documents after two disables");
    });

    it("validates parameters correctly", function () {
        // Test internalProfilingMaxAbandonedWritesPerSecondPerDb.
        let result = adminDB.runCommand({setParameter: 1, internalProfilingMaxAbandonedWritesPerSecondPerDb: -1});
        assert.commandFailed(
            result,
            "Expected setting internalProfilingMaxAbandonedWritesPerSecondPerDb to -1 to fail",
        );

        // Test internalQueryGlobalProfilingLockDeadlineMs.
        result = adminDB.runCommand({setParameter: 1, internalQueryGlobalProfilingLockDeadlineMs: -1});
        assert.commandFailed(result, "Expected setting internalQueryGlobalProfilingLockDeadlineMs to -1 to fail");

        // Test valid boundary values (0 is allowed).
        result = adminDB.runCommand({setParameter: 1, internalProfilingMaxAbandonedWritesPerSecondPerDb: 0});
        assert.commandWorked(
            result,
            "Expected setting internalProfilingMaxAbandonedWritesPerSecondPerDb to 0 to succeed",
        );

        result = adminDB.runCommand({setParameter: 1, internalQueryGlobalProfilingLockDeadlineMs: 0});
        assert.commandWorked(result, "Expected setting internalQueryGlobalProfilingLockDeadlineMs to 0 to succeed");

        // Reset to reasonable values.
        assert.commandWorked(
            adminDB.runCommand({
                setParameter: 1,
                internalProfilingMaxAbandonedWritesPerSecondPerDb: maxAbandonedWritesPerSecond,
            }),
        );
        assert.commandWorked(adminDB.runCommand({setParameter: 1, internalQueryGlobalProfilingLockDeadlineMs: 1}));
    });

    it("disables profiling immediately with zero tolerance threshold", function () {
        const testDB = conn.getDB(jsTestName() + "_zeroThreshold");
        assert.commandWorked(testDB.dropDatabase());
        const coll = testDB.getCollection("testColl");

        // Enable profiling.
        assert.commandWorked(testDB.setProfilingLevel(2));

        // Set threshold to 0 (zero tolerance for abandoned writes per second).
        assert.commandWorked(
            adminDB.runCommand({setParameter: 1, internalProfilingMaxAbandonedWritesPerSecondPerDb: 0}),
        );

        const stats1 = getProfilerStats();
        const initialDbsPastThreshold = stats1.dbsPastThreshold;

        // First, perform a non-problematic write (should not disable profiling, even with a zero
        // threshold).
        assert.commandWorked(coll.insert({x: 1}));
        // Profiling level should still be 2 as no problematic writes have occurred.
        let profilingStatusBefore = testDB.runCommand({profile: -1});
        assert.eq(profilingStatusBefore.was, 2);

        // Do a single problematic write - even one failure should disable profiling.
        doSomeProblematicProfiling(coll, 1, "test6-threshold-zero");

        // Verify profiling level is 0.
        let profilingStatus = testDB.runCommand({profile: -1});
        assert.eq(profilingStatus.was, 0, "Expected profiling level to be 0 with threshold of 0");

        // Verify profiling is disabled.
        const stats2 = getProfilerStats();
        assert.eq(
            stats2.dbsPastThreshold,
            initialDbsPastThreshold + 1,
            "Expected dbsPastThreshold to increment with threshold of 0",
        );
        assertNoteDocExists(testDB);

        // Reset threshold.
        assert.commandWorked(
            adminDB.runCommand({
                setParameter: 1,
                internalProfilingMaxAbandonedWritesPerSecondPerDb: maxAbandonedWritesPerSecond,
            }),
        );
    });
});
