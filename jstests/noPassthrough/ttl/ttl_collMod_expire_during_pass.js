/**
 * Verify that a collMod which mutates 'expireAfterSeconds' while the
 * TTLMonitor is paused mid-pass is observed by the next pass and the
 * pass after that, with no stale info-map entry resurfacing.
 *
 * Coverage gap surfaced by SERVER-97661 jstest audit (gap #2).
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 1}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("ttl_collMod_expire_during_pass");
const coll = db.getCollection(jsTestName());
coll.drop();

// Seed two documents whose timestamps are ~1 hour in the past, then create
// a TTL index with a long expiration (1 day) so the docs are NOT yet
// eligible.
const past = new Date(Date.now() - 3600 * 1000);
assert.commandWorked(coll.insert([{_id: 0, t: past}, {_id: 1, t: past}]));
assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: 86400, name: "t_ttl"}));

// Let one pass complete so the TTL info map is populated.
TTLUtil.waitForPass(db);
assert.eq(2, coll.countDocuments({}), "no reap expected with 1d expiry");

// Freeze the TTL monitor between passes. This deterministically opens a
// window where collMod can mutate expireAfterSeconds without racing the
// monitor.
const pauseTtl = configureFailPoint(primary, "hangTTLMonitorBetweenPasses");
pauseTtl.wait();

// Flip the expiry to zero. Both docs (~1h in the past) are now eligible.
assert.commandWorked(db.runCommand({
    collMod: coll.getName(),
    index: {name: "t_ttl", expireAfterSeconds: 0},
}));

// Confirm catalog reflects the new expiry before unblocking the monitor.
const idxes = coll.getIndexes().filter((i) => i.name === "t_ttl");
assert.eq(1, idxes.length, idxes);
assert.eq(0, idxes[0].expireAfterSeconds, idxes[0]);

// Release the monitor; assert reaping happens within two passes.
pauseTtl.off();
TTLUtil.waitForPass(db);
TTLUtil.waitForPass(db);

assert.soon(() => coll.countDocuments({}) === 0,
            "expected TTLMonitor to reap both docs after collMod to expireAfterSeconds:0");

// Sanity: a third pass must not log anything indicating the old (1d) entry
// is still present in the TTL info map. serverStatus().metrics.ttl.passes
// continues to advance, which is sufficient evidence the monitor did not
// crash on a stale entry.
const passesBefore = db.serverStatus().metrics.ttl.passes;
TTLUtil.waitForPass(db);
assert.gt(db.serverStatus().metrics.ttl.passes, passesBefore);

rst.stopSet();
