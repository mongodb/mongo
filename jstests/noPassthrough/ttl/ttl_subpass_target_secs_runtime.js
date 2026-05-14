/**
 * Verify that 'ttlMonitorSubPassTargetSecs' can be adjusted at runtime and continues to bound
 * the sub-pass loop without stranding documents. With a small sub-pass target on a multi-index
 * workload, the monitor must still drive all collections to zero over consecutive passes — the
 * knob bounds per-sub-pass effort, it does not skip indexes.
 *
 * SERVER-96179: extends TTL unit testing to exercise the sub-pass scheduler.
 */
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        ttlMonitorSleepSecs: 1,
        ttlMonitorBatchDeletes: true,
    },
});
const db = conn.getDB("test");

// Pause the monitor so multi-collection seed lands atomically.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: false}));

const collNames = ["ttl_sub_a", "ttl_sub_b", "ttl_sub_c"];
const docsPerColl = 60;
const past = new Date(new Date().getTime() - 3600 * 1000 * 24);

for (const name of collNames) {
    const c = db[name];
    c.drop();
    assert.commandWorked(c.createIndex({x: 1}, {expireAfterSeconds: 0}));
    const bulk = c.initializeUnorderedBulkOp();
    for (let i = 0; i < docsPerColl; i++) {
        bulk.insert({x: past, i: i});
    }
    assert.commandWorked(bulk.execute());
    assert.eq(docsPerColl, c.find().itcount(), name + " seed count");
}

// Set the smallest legal sub-pass target (0 means each TTL index iterated exactly once per
// sub-pass) and resume.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorSubPassTargetSecs: 0}));
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorEnabled: true}));

// Each pass must visit every collection at least once; eventually all must drain.
assert.soon(
    function () {
        for (const name of collNames) {
            if (db[name].find().itcount() != 0) return false;
        }
        return true;
    },
    "All TTL indexes must drain under ttlMonitorSubPassTargetSecs=0",
    60 * 1000,
);

// Raise the sub-pass target while the monitor is active and confirm new inserts also drain. This
// covers the on-update path where the knob changes mid-flight.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorSubPassTargetSecs: 30}));

for (const name of collNames) {
    const c = db[name];
    const bulk = c.initializeUnorderedBulkOp();
    for (let i = 0; i < 20; i++) {
        bulk.insert({x: past, i: i});
    }
    assert.commandWorked(bulk.execute());
}

// Wait for one full pass then verify drain.
TTLUtil.waitForPass(db);
assert.soon(
    function () {
        for (const name of collNames) {
            if (db[name].find().itcount() != 0) return false;
        }
        return true;
    },
    "Second-round inserts must drain after raising ttlMonitorSubPassTargetSecs",
    60 * 1000,
);

// Sanity: the parameter rejects negative values.
assert.commandFailedWithCode(
    db.adminCommand({setParameter: 1, ttlMonitorSubPassTargetSecs: -1}),
    ErrorCodes.BadValue,
);

MongoRunner.stopMongod(conn);
