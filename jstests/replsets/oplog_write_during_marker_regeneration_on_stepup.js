/**
 * SERVER-126515: Investigates what happens when an oplog write occurs while a node is
 * generating initial oplog truncate markers during step-up.
 *
 * Scenario:
 *   1. Start a 2-node replica set, do initial writes against the primary.
 *   2. Restart the secondary with the `hangDuringOplogSampling` failpoint set to alwaysOn so
 *      that the OplogCapMaintainer thread blocks inside initial marker sampling.
 *   3. Step the (still-sampling) secondary up to primary.
 *   4. While marker regeneration is paused on the new primary, drive concurrent oplog writes
 *      against it.
 *   5. Release the failpoint and confirm:
 *        - The concurrent writes are durably in the oplog of the new primary.
 *        - Initial sampling completes (log 22382 emitted on the new primary).
 *        - Subsequent inserts large enough to roll past `oplogSize` cause truncation, i.e.
 *          the markers produced after step-up correctly bound the oplog.
 *
 * Worst case described on the ticket is that the in-flight writes are not factored into the
 * first marker spacing and we truncate slightly later than expected. This test pins the
 * observed behavior so future regressions are caught.
 *
 * @tags: [requires_replication, requires_persistence, requires_majority_read_concern]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";

const oplogSizeMB = 1;
const largeStr = "a".repeat(450 * 1024); // ~450 KiB per insert => ~2 docs fills the cap
const dbName = "test";
const collName = "markers_stepup";

const rst = new ReplSetTest({
    name: "oplog_write_during_marker_regen_on_stepup",
    nodes: 2,
    oplogSize: oplogSizeMB,
    nodeOptions: {
        syncdelay: 1,
        setParameter: {
            logComponentVerbosity: tojson({storage: 1, replication: 1}),
            minOplogTruncationPoints: 2,
            internalQueryExecYieldPeriodMS: 86400000, // Disable yielding.
        },
    },
});
rst.startSet();
rst.initiate();

const oldPrimary = rst.getPrimary();
const oldSecondary = rst.getSecondary();

// Use w:1 default so writes against the new primary do not block on the (about-to-be) other node.
assert.commandWorked(
    oldPrimary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Seed the oplog with a few small docs so the secondary has something to sample.
jsTest.log.info("Seeding initial documents via the original primary.");
let nextId = 0;
for (let i = 0; i < 10; i++) {
    assert.commandWorked(
        oldPrimary.getDB(dbName).getCollection(collName).insert({_id: nextId++}, {writeConcern: {w: "majority"}}),
    );
}
rst.awaitReplication();

// Restart the secondary with the sampling failpoint armed. When it comes back up,
// the OplogCapMaintainer thread will hang inside initial marker sampling.
jsTest.log.info("Restarting secondary with hangDuringOplogSampling=alwaysOn.");
rst.restart(oldSecondary, {
    setParameter: {"failpoint.hangDuringOplogSampling": tojson({mode: "alwaysOn"})},
});

// Wait for the restarted node to become available and confirm it is hung inside sampling.
const restartedSecondary = rst.nodes[rst.getNodeId(oldSecondary)];
rst.awaitSecondaryNodes(null, [restartedSecondary]);

assert.commandWorked(
    restartedSecondary.adminCommand({
        waitForFailPoint: "hangDuringOplogSampling",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);
jsTest.log.info("Confirmed restarted secondary is paused inside initial oplog sampling.");

// Step up the (still-sampling) secondary. Step-up itself does not block on marker creation,
// so the node should win the election while the cap maintainer thread remains paused.
jsTest.log.info("Stepping up the still-sampling secondary.");
// Stop replication source side first so the old primary cannot keep applying competing writes.
rst.stepUp(restartedSecondary, {awaitReplicationBeforeStepUp: false});
assert.eq(restartedSecondary, rst.getPrimary(), "Expected restartedSecondary to be the new primary.");

// Re-assert the failpoint is still held on the new primary post-step-up.
assert.commandWorked(
    restartedSecondary.adminCommand({
        waitForFailPoint: "hangDuringOplogSampling",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);
jsTest.log.info("New primary is up while initial marker sampling is still paused.");

// Drive concurrent oplog writes against the new primary in a background thread while
// marker regeneration is paused. Two large docs so size > oplogSize => any reasonable
// marker placement must include them.
const concurrentIds = [nextId++, nextId++];
const writer = new Thread(
    function (host, dbName, collName, ids, largeStr) {
        const conn = new Mongo(host);
        const coll = conn.getDB(dbName).getCollection(collName);
        for (const id of ids) {
            assert.commandWorked(coll.insert({_id: id, longString: largeStr}));
        }
        return "ok";
    },
    restartedSecondary.host,
    dbName,
    collName,
    concurrentIds,
    largeStr,
);
writer.start();
writer.join();
assert.eq(writer.returnData(), "ok", "Concurrent writer did not complete cleanly.");
jsTest.log.info("Concurrent writes against the new primary completed: " + tojson(concurrentIds));

// The writes must already be visible in the oplog while sampling is still paused — this is
// the answer to the ticket's first question: oplog writes are not blocked by marker regen.
const newPrimaryOplog = restartedSecondary.getDB("local").getCollection("oplog.rs");
assert.soon(() => {
    const seen = new Set(
        newPrimaryOplog
            .find({ns: dbName + "." + collName, "o._id": {$in: concurrentIds}}, {"o._id": 1})
            .toArray()
            .map((d) => d.o._id),
    );
    return concurrentIds.every((id) => seen.has(id));
}, "Concurrent writes never appeared in the new primary's oplog.");

// Force a checkpoint so the on-disk oplog catches up with the in-memory state before we
// release the failpoint. This is what an operator restart would observe.
assert.commandWorked(restartedSecondary.getDB("admin").runCommand({fsync: 1}));

// Release the failpoint and assert marker creation completes.
jsTest.log.info("Releasing hangDuringOplogSampling on the new primary.");
assert.commandWorked(
    restartedSecondary.adminCommand({configureFailPoint: "hangDuringOplogSampling", mode: "off"}),
);

// Log 22382 is emitted at the end of marker construction. If sampling factored the
// concurrent writes in at all, it does so here; otherwise it produced markers from the
// pre-step-up snapshot and the new writes get accounted for at next eviction.
checkLog.containsJson(restartedSecondary, 22382);
jsTest.log.info("Initial oplog marker construction finished on the new primary.");

// Drive enough additional inserts to roll the oplog past its cap. Truncation must
// eventually remove the early seed docs, proving the markers produced under this race
// are still functional (worst-case in the ticket: 'truncate slightly later than expected').
jsTest.log.info("Inserting additional large documents to force truncation.");
const newPrimaryColl = restartedSecondary.getDB(dbName).getCollection(collName);
for (let i = 0; i < 50; i++) {
    assert.commandWorked(newPrimaryColl.insert({_id: nextId++, longString: largeStr}));
}
assert.commandWorked(restartedSecondary.getDB("admin").runCommand({fsync: 1}));

// The earliest seed docs (_id: 0..9) must eventually be truncated out of the new primary's
// oplog. If marker regeneration on step-up produced a degenerate marker set, truncation
// would stall and this assert.soon would time out — that is the regression we are pinning.
assert.soon(
    () => {
        try {
            const stillThere = newPrimaryOplog.countDocuments({ns: dbName + "." + collName, "o._id": {$lt: 10}});
            return stillThere === 0;
        } catch (e) {
            if (e.code !== ErrorCodes.CappedPositionLost) {
                throw e;
            }
            return false;
        }
    },
    "Initial seed docs were never truncated post-step-up; marker regeneration likely produced a bad marker set.",
);
jsTest.log.info("Truncation proceeded normally post-step-up — markers are healthy.");

rst.stopSet();
