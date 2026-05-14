/**
 * SERVER-97659: Reproduce the timing condition where the underlying collection is
 * dropped AFTER the TTL monitor has begun _doTTLIndexDelete for that collection. We
 * use hangTTLMonitorWithLock to pin the monitor inside the per-collection critical
 * section, drop the collection from another client, and assert that the monitor
 * either:
 *   (a) sees collection.exists() == false after re-acquiring the catalog snapshot
 *       and returns false cleanly, OR
 *   (b) sees the UUID mismatch and returns false cleanly.
 *
 * Either path must NOT crash the server and MUST NOT count as an
 * invalidTTLIndexSkips error.
 *
 * Timing model:
 *   _doTTLIndexDelete -> acquireCollection MODE_IX -> FAILPOINT HANG ->
 *   dropCollection (another client) -> failpoint released ->
 *   !coll.exists() || coll.uuid() != uuid -> return false.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 1}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const admin = primary.getDB("admin");
const testDB = primary.getDB("test");
const coll = testDB.ttl_coll_dropped_mid_pass;
coll.drop();

// Seed expired docs and create a TTL index while the monitor is paused so we
// deterministically know the index exists at the start of the next pass.
const pauseBetween = configureFailPoint(primary, "hangTTLMonitorBetweenPasses");
pauseBetween.wait();

const past = new Date(new Date().getTime() - 3600 * 1000 * 24);
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 200; i++) {
    bulk.insert({x: past, i: i});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 60}));

// Arm mid-pass hang, then release the inter-pass hang. The monitor will enter
// _doTTLIndexDelete, acquire MODE_IX on our collection, and stop at the failpoint.
const hangWithLock = configureFailPoint(primary, "hangTTLMonitorWithLock");
pauseBetween.off();
hangWithLock.wait();

// Capture metrics before we drop. invalidTTLIndexSkips must not be incremented by a
// vanished collection: the index was conforming, the collection just disappeared.
const passesBefore = admin.serverStatus().metrics.ttl.passes;
const invalidSkipsBefore = admin.serverStatus().metrics.ttl.invalidTTLIndexSkips;

// Drop the collection while the monitor holds MODE_IX. The drop will queue behind
// the IX lock until the failpoint is released.
const dropAwait = startParallelShell(function () {
    assert.commandWorked(db.getSiblingDB("test").runCommand({drop: "ttl_coll_dropped_mid_pass"}));
}, primary.port);

// Yield the failpoint so the monitor's lock is released and the queued drop runs.
hangWithLock.off();
dropAwait();

// The monitor must complete at least one more pass cleanly.
assert.soon(
    () => admin.serverStatus().metrics.ttl.passes >= passesBefore + 1,
    "TTL monitor did not advance after collection drop mid-pass",
);

// The collection is gone, so itcount is zero — but the disappearance must come from
// the drop, not from TTL reaping. The crucial invariant is that the monitor did not
// crash and did not flag the index as invalid.
assert.eq(false, coll.exists(), "Collection should be dropped");

const invalidSkipsAfter = admin.serverStatus().metrics.ttl.invalidTTLIndexSkips;
assert.eq(
    invalidSkipsBefore,
    invalidSkipsAfter,
    "Dropping the collection mid-pass must not increment invalidTTLIndexSkips",
);

// Sanity: the TTL monitor thread is still alive and advancing passes.
const passesMid = admin.serverStatus().metrics.ttl.passes;
assert.soon(
    () => admin.serverStatus().metrics.ttl.passes > passesMid,
    "TTL monitor thread did not survive a mid-pass collection drop",
);

rst.stopSet();
