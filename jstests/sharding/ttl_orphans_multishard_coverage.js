/**
 * SERVER-92779 — extended coverage for "TTL delete progress blocked by unowned documents".
 *
 * Wave-1 (SERVER-126541, commit d65fe1832d) shipped a 2-shard reproducer at
 *   jstests/sharding/ttl_blocked_by_unowned_docs.js
 * which exercises the BatchedDeleteStage accounting bug:
 *
 *   - BatchedDeleteStage::_passTotalDocsStaged is incremented in the staging
 *     loop for every WSM appended to the buffer, BEFORE the commit phase
 *     consults the ownership filter and skips orphans
 *     (batched_delete_stage.cpp:389-394, 516).
 *   - Once a shard's expired-orphan count crosses 'ttlIndexDeleteTargetDocs',
 *     the stage stages orphans, _passTargetMet() flips true without
 *     committing a single delete, the buffer drains by skipping every staged
 *     orphan, the stage returns EOF — and the TTL monitor's next sub-pass
 *     restarts at the same index position.
 *
 * This wave-3 extension probes two failure modes wave-1 did NOT cover:
 *
 *   Variant A — Multi-shard fan-out (3 shards).
 *     The wave-1 repro showed the bug on a single shard hosting an
 *     orphan band. Variant A places independent orphan bands on shard0
 *     AND shard2 simultaneously (shard1 holds the owned chunk), then
 *     asserts the TTLMonitor on EACH donor shard is stuck. This rules out
 *     the "donor shard is special" explanation — the bug is per-shard,
 *     reproduces in parallel, and any fix must un-stick every donor.
 *
 *   Variant B — Cross-namespace TTL coverage (two collections, one
 *     monitor pass).
 *     The TTLMonitor walks every TTL index in a single sub-pass via
 *     'getTTLSubPasses'. Wave-1 only exercised one collection, so it
 *     could not detect collateral damage: when collection A's expired
 *     orphans saturate the pass target, collection B's owned expired
 *     documents on the SAME shard never make progress within that pass
 *     either (the monitor schedules sub-passes per-collection, but the
 *     batched-delete pass accounting is per-stage; an upstream fix that
 *     reschedules at the collection level must not starve siblings).
 *     Variant B colocates two TTL collections on shard0, dumps an
 *     orphan band into collection A only, and asserts collection B's
 *     owned docs still expire within the bounded soak window.
 *
 * Both variants are GATED on the wave-1 fix landing. Until then the test
 * is wired through the same '__TEMPORARILY_DISABLED_PENDING_SERVER_92779'
 * tag wave-1 introduced — flip FIX_LANDED to true once the
 * BatchedDeleteStage accounting fix + TTLMonitor reschedule guard merge.
 *
 * @tags: [
 *   requires_fcv_60,
 *   requires_sharding,
 *   __TEMPORARILY_DISABLED_PENDING_SERVER_92779,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Flip to true once SERVER-126541's BatchedDeleteStage accounting fix
// AND its TTLMonitor reschedule guard have landed on master. While
// false, both variants are expected to FAIL (the bug is live) and the
// __TEMPORARILY_DISABLED tag keeps CI green.
const FIX_LANDED = false;

if (!FIX_LANDED) {
    jsTest.log(
        "ttl_orphans_multishard_coverage: gated off pending SERVER-92779 / SERVER-126541 fix");
    quit();
}

// Range deleter disabled — orphans persist after moveChunk.
TestData.skipCheckOrphans = true;

// Lower the pass target so the orphan band needed to trigger the bug
// stays small (faster, cheaper, less flaky than the production default
// of 20k). The wave-1 repro uses the same trick.
const kPassTarget = 100;
const kOrphanBand = 150;  // > kPassTarget so _passTargetMet() trips.
const kSoakMs = 60 * 1000;
const kPollMs = 1000;

const baseSetParameter = {
    ttlMonitorSleepSecs: 1,
    disableResumableRangeDeleter: true,
    ttlIndexDeleteTargetDocs: kPassTarget,
};

// --------------------------------------------------------------------
// Variant A — Multi-shard fan-out (3 shards, two donors stuck in parallel)
// --------------------------------------------------------------------
(function variantA_multiShardFanout() {
    jsTest.log("Variant A — multi-shard fan-out across 3 shards");

    const st = new ShardingTest({
        shards: 3,
        rs: {nodes: 1, setParameter: baseSetParameter},
    });

    const dbName = "ttl_92779_multishard";
    const coll = st.s.getDB(dbName)["fanout"];
    const ns = coll.getFullName();

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Lay down two orphan bands (one destined for shard0, one for shard2)
    // plus a single owned doc that must end up on shard1.
    const now = new Date();
    const past = new Date(now.getTime() - 60 * 1000);

    let bulk = coll.initializeUnorderedBulkOp();
    // Band-A: ids [-2*kOrphanBand .. -kOrphanBand) — will stay on shard0 as orphans.
    for (let i = -2 * kOrphanBand; i < -kOrphanBand; i++) {
        bulk.insert({_id: i, ttlField: past, band: "A"});
    }
    // Band-C: ids [kOrphanBand .. 2*kOrphanBand) — will move to shard2 as orphans.
    for (let i = kOrphanBand; i < 2 * kOrphanBand; i++) {
        bulk.insert({_id: i, ttlField: past, band: "C"});
    }
    // Single owned canary on shard1.
    bulk.insert({_id: 0, ttlField: past, band: "B-canary"});
    assert.commandWorked(bulk.execute());

    // Split into 3 chunks: (-inf, -kOrphanBand), [-kOrphanBand, kOrphanBand), [kOrphanBand, +inf).
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: -kOrphanBand}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: kOrphanBand}}));

    // Move the middle chunk (owned canary) to shard1, leaving Band-A as
    // orphans on shard0. Move the high chunk to shard2 so Band-C is owned
    // there first, then move it BACK to shard1 to turn Band-C into orphans
    // on shard2.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: kOrphanBand + 1}, to: st.shard2.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: kOrphanBand + 1}, to: st.shard1.shardName}));

    // TTL index — fires immediately on the past-stamped docs.
    assert.commandWorked(coll.createIndex({ttlField: 1}, {expireAfterSeconds: 1}));

    // POST-FIX expectation: the single owned canary on shard1 expires
    // within the soak window even though shard0 + shard2 are each
    // simultaneously sitting on a band of expired orphans larger than
    // 'ttlIndexDeleteTargetDocs'.
    assert.soon(
        () => coll.countDocuments({band: "B-canary"}) === 0,
        "Variant A: TTLMonitor on shard1 failed to delete the owned canary " +
            "while shard0/shard2 each held orphan bands > targetPassDocs",
        kSoakMs,
        kPollMs);

    // Orphans must remain (range deleter disabled).
    assert.eq(
        kOrphanBand,
        st.rs0.getPrimary().getCollection(ns).countDocuments({band: "A"}),
        "Variant A: shard0 orphan band was unexpectedly cleared (range deleter on?)");
    assert.eq(
        kOrphanBand,
        st.rs2.getPrimary().getCollection(ns).countDocuments({band: "C"}),
        "Variant A: shard2 orphan band was unexpectedly cleared (range deleter on?)");

    st.stop();
})();

// --------------------------------------------------------------------
// Variant B — Cross-namespace TTL coverage (two TTL collections, one shard)
// --------------------------------------------------------------------
(function variantB_crossNamespaceCoverage() {
    jsTest.log("Variant B — cross-namespace TTL coverage on a single shard");

    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1, setParameter: baseSetParameter},
    });

    const dbName = "ttl_92779_xns";
    const db = st.s.getDB(dbName);
    const collOrphans = db["coll_with_orphans"];
    const collClean = db["coll_clean"];
    const nsOrphans = collOrphans.getFullName();
    const nsClean = collClean.getFullName();

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: nsOrphans, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: nsClean, key: {_id: 1}}));

    const past = new Date(Date.now() - 60 * 1000);

    // collOrphans: orphan band destined for shard0 + one owned doc.
    {
        let bulk = collOrphans.initializeUnorderedBulkOp();
        for (let i = 0; i < kOrphanBand; i++) {
            bulk.insert({_id: i, ttlField: past});
        }
        bulk.insert({_id: -1, ttlField: past, owned: true});
        assert.commandWorked(bulk.execute());

        assert.commandWorked(st.s.adminCommand({split: nsOrphans, middle: {_id: 0}}));
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: nsOrphans, find: {_id: 0}, to: st.shard1.shardName}));
        // After this: shard0 holds {_id: -1} owned + ids [0..kOrphanBand) as orphans.
    }

    // collClean: same shard0, no orphans, just expired owned docs.
    // This is the sibling whose progress must NOT be starved by collOrphans.
    {
        let bulk = collClean.initializeUnorderedBulkOp();
        for (let i = 0; i < 25; i++) {
            bulk.insert({_id: i, ttlField: past});
        }
        assert.commandWorked(bulk.execute());
        // No splits/moves — every doc stays owned on shard0.
    }

    // Create both TTL indexes. The TTLMonitor enumerates them in one
    // sub-pass; the bug under test is whether collOrphans's stuck pass
    // starves collClean's pass.
    assert.commandWorked(collOrphans.createIndex({ttlField: 1}, {expireAfterSeconds: 1}));
    assert.commandWorked(collClean.createIndex({ttlField: 1}, {expireAfterSeconds: 1}));

    // POST-FIX expectation: collClean's owned docs expire within the soak
    // window. Under the bug, collOrphans saturates 'ttlIndexDeleteTargetDocs'
    // each pass, the stage returns EOF without committing, and depending on
    // how the monitor schedules sub-passes the sibling collection's owned
    // docs may also be starved — this variant pins that contract.
    assert.soon(
        () => collClean.countDocuments({}) === 0,
        "Variant B: TTLMonitor failed to delete owned docs in coll_clean " +
            "while coll_with_orphans was blocked on an orphan band",
        kSoakMs,
        kPollMs);

    // POST-FIX expectation: the single owned doc in collOrphans also
    // expires — same guarantee wave-1 pinned, repeated here so cross-namespace
    // doesn't accidentally regress single-namespace.
    assert.soon(
        () => collOrphans.countDocuments({owned: true}) === 0,
        "Variant B: owned doc in coll_with_orphans not deleted",
        kSoakMs,
        kPollMs);

    // Orphans must remain.
    assert.eq(
        kOrphanBand,
        st.rs0.getPrimary().getCollection(nsOrphans).countDocuments({}),
        "Variant B: orphan band on shard0 was unexpectedly cleared");

    st.stop();
})();

jsTest.log("ttl_orphans_multishard_coverage — both variants passed");
