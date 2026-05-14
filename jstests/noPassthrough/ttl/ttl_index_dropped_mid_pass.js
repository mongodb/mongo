/**
 * SERVER-97659: Reproduce the timing condition where a TTL index is dropped AFTER
 * _doTTLIndexDelete has begun (and the lock has been acquired in _doTTLIndexDelete via
 * hangTTLMonitorWithLock) but BEFORE _deleteExpiredWithIndex's call to getValidTTLIndex
 * runs. Under this timing, getValidTTLIndex finds the index entry missing and the TTL
 * monitor must abort the per-index pass cleanly without crashing.
 *
 * Timing model:
 *   _doTTLSubPass -> _doTTLIndexDelete -> [acquire MODE_IX, FAILPOINT HANG] ->
 *                    dropIndex (from another client) ->
 *                    _deleteExpiredWithIndex -> getValidTTLIndex returns nullptr ->
 *                    LOGV2_DEBUG(22535, "index not found; skipping ttl job")
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
const coll = testDB.ttl_drop_mid_pass;
coll.drop();

// Seed an unclustered collection with expired documents.
const past = new Date(new Date().getTime() - 3600 * 1000 * 24);
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 200; i++) {
    bulk.insert({x: past, i: i});
}
assert.commandWorked(bulk.execute());
assert.eq(200, coll.find().itcount());

// Block the monitor BEFORE it picks up any indexes, then create the TTL index. This
// guarantees that the index will be visible on the very next pass.
const pauseBetween = configureFailPoint(primary, "hangTTLMonitorBetweenPasses");
pauseBetween.wait();
assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 60}));

// Arm the mid-pass hang BEFORE we release the inter-pass hang, so we deterministically
// catch the monitor while it holds MODE_IX inside _doTTLIndexDelete for our collection.
const hangWithLock = configureFailPoint(primary, "hangTTLMonitorWithLock");
pauseBetween.off();
hangWithLock.wait();

// The monitor is now inside _doTTLIndexDelete with the lock held. Drop the index on
// another connection. After we release the failpoint, _deleteExpiredWithIndex will run
// and getValidTTLIndex must return nullptr (the index entry no longer exists), causing
// the pass to be skipped cleanly.
const passesBefore = admin.serverStatus().metrics.ttl.passes;
const invalidSkipsBefore = admin.serverStatus().metrics.ttl.invalidTTLIndexSkips;
assert.commandWorked(coll.dropIndex({x: 1}));

hangWithLock.off();

// The monitor must complete at least one more pass without crashing.
assert.soon(
    () => admin.serverStatus().metrics.ttl.passes >= passesBefore + 1,
    "TTL monitor did not advance after dropping a TTL index mid-pass",
);

// The collection itself should be intact: no docs deleted on this pass because the
// index was dropped before _deleteExpiredWithIndex ran the bounded plan.
assert.eq(200, coll.find().itcount(), "TTL monitor must not delete docs after the index disappears");

// Validate that the monitor did NOT count this as an invalid-index skip (the index was
// gone, not malformed). invalidTTLIndexSkips is reserved for non-conformant indexes.
const invalidSkipsAfter = admin.serverStatus().metrics.ttl.invalidTTLIndexSkips;
assert.eq(
    invalidSkipsBefore,
    invalidSkipsAfter,
    "Dropping a TTL index mid-pass must not increment invalidTTLIndexSkips",
);

rst.stopSet();
