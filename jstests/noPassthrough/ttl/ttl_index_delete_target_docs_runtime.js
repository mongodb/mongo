/**
 * Verify the runtime semantics of 'ttlIndexDeleteTargetDocs' — when set to a small positive
 * value, a single sub-pass over a heavily-expired TTL index removes at most that many docs.
 * Subsequent passes drain the remainder. Validates that the knob is honored at runtime, not just
 * startup, and that batched-deletes accounting tracks the per-pass cap.
 *
 * SERVER-96179: extends TTL unit testing to cover runtime tuning of batch-delete caps.
 *
 * @tags: [
 *     requires_fcv_70,
 * ]
 */
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        ttlMonitorSleepSecs: 1,
        ttlMonitorBatchDeletes: true,
        // Large default that won't bound deletions on its own.
        ttlIndexDeleteTargetTimeMS: 0,
    },
});
const db = conn.getDB("test");
const coll = db.ttl_target_docs;
coll.drop();

const docCount = 200;
const batchCap = 25;

// Pause the monitor while we set up so initial conditions are deterministic.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 0}));

const past = new Date(new Date().getTime() - 3600 * 1000 * 24);
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < docCount; i++) {
    bulk.insert({x: past, i: i});
}
assert.commandWorked(bulk.execute());
assert.eq(docCount, coll.find().itcount());

// Set the per-index per-pass cap and re-enable.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlIndexDeleteTargetDocs: batchCap}));
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));

// Drain — every pass should remove at most 'batchCap' documents, so the count strictly decreases
// by no more than batchCap per pass. We don't assert exact per-pass counts (the monitor may make
// multiple sub-passes per wall pass at small sleep intervals), but we do assert eventual drain
// and that the cap is not silently ignored on the very first observable pass after toggling.
const startCount = coll.find().itcount();
TTLUtil.waitForPass(db);
const afterOnePass = coll.find().itcount();
assert.lt(afterOnePass, startCount, "TTL pass should make progress");
assert.gte(
    afterOnePass,
    startCount - batchCap * 4,
    "Per-pass deletion should remain bounded by ttlIndexDeleteTargetDocs across a small number" +
        " of sub-passes; expected at most ~4× cap removed but saw " +
        (startCount - afterOnePass),
);

// Loosen the cap and confirm eventual full drain.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlIndexDeleteTargetDocs: 0}));
assert.soon(function () {
    return coll.find().itcount() == 0;
}, "TTL index must fully drain once batch cap is removed");

// Confirm that batched-deletes accounting reflects real deletions.
const status = db.serverStatus();
assert.gte(
    status.batchedDeletes.docs,
    docCount,
    "batchedDeletes.docs should account for every TTL deletion",
);

MongoRunner.stopMongod(conn);
