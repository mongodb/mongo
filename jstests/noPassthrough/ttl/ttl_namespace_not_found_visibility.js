/**
 * Repros SERVER-88352: a TTL deleter pass races a concurrent collection drop, the catalog
 * UUID->NSS lookup returns boost::none, and TTLMonitor::_doTTLIndexDelete silently
 * deregisters + returns false. Operators get no signal that a TTL fire was skipped, why it
 * was skipped, or which namespace went missing -- the only observable is "deletedDocuments
 * did not grow." This conflates drop-races, sharding-filter-metadata gaps (the configShard
 * window described in the SERVER-88352 description), and benign no-op passes.
 *
 * This test pre-registers the proposed observability shape so a future fix has a target:
 *
 *   db.serverStatus().metrics.ttl.skipReasons = {
 *     namespaceNotFound: <Counter64>,   // UUID->NSS lookup returned boost::none
 *     orphan:            <Counter64>,   // SERVER-92779 territory (kept distinct on purpose)
 *     other:             <Counter64>,   // residual catch-all -- should stay near zero
 *   }
 *
 *   db.adminCommand({currentOp: 1, "desc": "TTLMonitor"})[0].ttl = {
 *     skipReasons: { namespaceNotFound, orphan, other },  // pass-scoped histogram
 *     lastSkip:    { uuid, reason, ts },                  // most recent skip for triage
 *   }
 *
 * Today the test asserts what we *do* see (drop racing TTL → log line + no metric growth)
 * and records what we *want* to see (the histogram). When the fix lands, the `.skip(...)`
 * guards flip to live assertions without touching the repro scaffold.
 *
 * Distinct from SERVER-92779 (orphan-doc visibility on sharded collections). Both surfaces
 * are needed; this ticket owns the drop-race + configShard-window axis only.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_60,
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
const testDB = primary.getDB("ttl_nsnotfound_visibility");
const coll = testDB.victim;

// Pin TTL pass to a known boundary so the drop can race the *next* pass deterministically.
const hangBetweenPasses = configureFailPoint(primary, "hangTTLMonitorBetweenPasses");
hangBetweenPasses.wait();

assert.commandWorked(coll.createIndex({createdAt: 1}, {expireAfterSeconds: 0}));
assert.commandWorked(coll.insert({createdAt: new Date(0)}));   // already expired
assert.commandWorked(coll.insert({createdAt: new Date(0)}));

// Snapshot of metrics BEFORE we introduce the race. Capture what's there today so the
// before/after delta tells us whether the new counters actually moved.
const before = assert.commandWorked(admin.runCommand({serverStatus: 1})).metrics.ttl;
jsTest.log(`ttl metrics before race: ${tojson(before)}`);

// Race: drop the collection while the TTL monitor is parked. When we release the fail
// point, the TTLCollectionCache still has an Info entry keyed by the old UUID, the
// catalog lookup returns boost::none, and _doTTLIndexDelete hits the silent path at
// src/mongo/db/ttl/ttl_monitor.cpp:460-469.
assert(coll.drop(), "expected drop() to succeed before TTL pass");

// Let exactly one pass through.
const passBefore = admin.serverStatus().metrics.ttl.passes;
assert.commandWorked(admin.runCommand(
    {configureFailPoint: "hangTTLMonitorBetweenPasses", mode: {skip: 1}}));
assert.soon(() => admin.serverStatus().metrics.ttl.passes >= passBefore + 1,
            "TTL monitor did not complete a pass after drop");

const after = assert.commandWorked(admin.runCommand({serverStatus: 1})).metrics.ttl;
jsTest.log(`ttl metrics after race: ${tojson(after)}`);

// ── Today's observable: nothing visible besides absence ─────────────────────────────────
// The TTL monitor swallows the missing-namespace case. There is no log on the silent path
// (the deregister-and-return-false branch), and no counter ticks. The only proof the
// monitor "saw" the drop is that `deletedDocuments` did not advance.
assert.eq(before.deletedDocuments,
          after.deletedDocuments,
          "TTL should not have deleted documents from a dropped collection");

// ── Pre-registered observability: serverStatus histogram ────────────────────────────────
// Skip until the fix lands. When `skipReasons` ships, this block flips to assertions and
// the test gates on namespaceNotFound > 0 + orphan == 0 + other == 0.
if (after.skipReasons === undefined) {
    jsTest.log("ttl.skipReasons not implemented yet (SERVER-88352); pre-registering shape");
} else {
    assert.gt(after.skipReasons.namespaceNotFound,
              before.skipReasons.namespaceNotFound,
              "namespaceNotFound counter must advance when UUID->NSS lookup fails mid-pass");
    assert.eq(after.skipReasons.orphan,
              before.skipReasons.orphan,
              "orphan counter is owned by SERVER-92779; must not move on a pure drop-race");
    assert.eq(after.skipReasons.other,
              before.skipReasons.other,
              "other should stay at zero for the canonical drop-race case");
}

// ── Pre-registered observability: currentOp surface ─────────────────────────────────────
// Operators reaching for currentOp during an incident want a per-monitor histogram + the
// most recent skip's (uuid, reason, ts) for triage. Same skip-until-fix pattern.
const cop = admin.aggregate([
    {$currentOp: {allUsers: true, idleConnections: true, localOps: true}},
    {$match: {desc: "TTLMonitor"}},
]).toArray();

if (cop.length === 0) {
    jsTest.log("TTLMonitor not exposed via currentOp yet (SERVER-88352); shape pre-registered");
} else {
    const ttlOp = cop[0].ttl;
    assert.neq(ttlOp, undefined, "TTLMonitor currentOp row must carry a 'ttl' sub-document");
    assert.gte(ttlOp.skipReasons.namespaceNotFound, 1);
    assert.eq(ttlOp.lastSkip.reason, "namespaceNotFound");
    assert.neq(ttlOp.lastSkip.uuid, undefined);
    assert.neq(ttlOp.lastSkip.ts, undefined);
}

hangBetweenPasses.off();
rst.stopSet();
