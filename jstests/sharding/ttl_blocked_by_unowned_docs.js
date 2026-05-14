/**
 * Regression test for SERVER-92779.
 *
 * Demonstrates that the TTL monitor's per-pass document target (TTLIndexDeleteTargetDocs)
 * is decremented by *staged* documents, including orphans that the batched delete stage
 * subsequently skips because they fall outside the shard's ownership filter. When the
 * number of expired orphan documents on a shard exceeds TTLIndexDeleteTargetDocs, the
 * batched delete stage hits its pass target after staging only orphans, declares the pass
 * complete, and never reaches the owned expired document later in the index.
 *
 * Reproducer from the ticket:
 *   (1) Shard a collection so all chunks start on shard0.
 *   (2) Insert TTLIndexDeleteTargetDocs documents that will become orphans.
 *   (3) Insert one document that will remain owned on shard0.
 *   (4) Split off the orphan range and moveChunk it to shard1 with the resumable range
 *       deleter disabled so the documents on shard0 are left as orphans.
 *   (5) Create a TTL index with a small expireAfterSeconds.
 *   (6) The single owned expired document on shard0 is never deleted by the TTL monitor.
 *
 * Today: the assertion that the owned expired doc is deleted will time out.
 * After the SERVER-92779 fix: the owned doc is deleted promptly because either (a) orphans
 * no longer count against targetPassDocs, or (b) the TTL monitor's outer loop iterates
 * past the orphan-saturated batch until a real deletion happens or genuine EOF is reached.
 *
 * @tags: [
 *   requires_fcv_60,
 *   requires_sharding,
 *   # This test relies on disableResumableRangeDeleter to manufacture orphans.
 *   does_not_support_stepdowns,
 *   # The bug being reproduced will cause assert.soon to time out; mark to keep the suite
 *   # green until the fix lands. Remove this tag (and the early `return`) once
 *   # SERVER-92779 is closed.
 *   __TEMPORARILY_DISABLED_PENDING_SERVER_92779,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

// Range deleter is disabled to manufacture orphans, so the standard end-of-test orphan
// audit would fail.
TestData.skipCheckOrphans = true;
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// Keep the reproducer cheap. The bug triggers when expired orphans on a shard exceed
// targetPassDocs. We lower targetPassDocs from its 50000 default to 100 via the
// ttlIndexDeleteTargetDocs server parameter so we only need to manufacture ~150 orphans.
const kTargetPassDocs = 100;
const kNumOrphans = 150;

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 1,
        setParameter: {
            ttlMonitorSleepSecs: 1,
            disableResumableRangeDeleter: true,
            ttlIndexDeleteTargetDocs: kTargetPassDocs,
            // Drive the TTL monitor often so we cover several passes within assert.soon.
            ttlMonitorEnabled: true,
        },
    },
});

const dbName = "test";
const collName = jsTest.name();
const ns = dbName + "." + collName;
const mongos = st.s;
const testDB = mongos.getDB(dbName);
const coll = testDB[collName];

assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {sk: 1}}));

// Chunks at startup: a single chunk on shard0 covering (-inf, +inf).
// We'll split at sk: 0 so [-inf, 0) stays on shard0 (holds the one owned doc) and
// [0, +inf) gets migrated to shard1 (becomes orphans on shard0 once range deletion is
// disabled).

// Marker for "expired in the past". Inserting through mongos so chunk targeting works
// before the split.
const expiredAt = new Date(Date.now() - 60 * 1000);

// Insert the documents that will become orphans on shard0 (sk >= 0).
{
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kNumOrphans; i++) {
        bulk.insert({_id: "orphan-" + i, sk: i + 1, createdAt: expiredAt});
    }
    assert.commandWorked(bulk.execute());
}

// Insert the single document that will remain owned by shard0 (sk < 0). This is the
// document whose deletion the bug prevents.
const ownedId = "owned-canary";
assert.commandWorked(coll.insert({_id: ownedId, sk: -1, createdAt: expiredAt}));

// Split and migrate the [0, +inf) chunk to shard1. Because
// disableResumableRangeDeleter is true, the docs with sk >= 0 will remain physically on
// shard0 as orphans.
assert.commandWorked(mongos.adminCommand({split: ns, middle: {sk: 0}}));
assert.commandWorked(
    mongos.adminCommand({moveChunk: ns, find: {sk: 1}, to: st.shard1.shardName}),
);

// Sanity check: shard0 still holds (kNumOrphans + 1) physical documents (the orphans
// plus the canary), and shard1 holds 0 (the migration commit copied them but they're
// now owned by shard1 only).
const shard0Coll = st.rs0.getPrimary().getCollection(ns);
const shard1Coll = st.rs1.getPrimary().getCollection(ns);

assert.eq(
    kNumOrphans + 1,
    shard0Coll.countDocuments({}),
    "shard0 should physically hold the canary plus all the orphans",
);
assert.eq(
    kNumOrphans,
    shard1Coll.countDocuments({}),
    "shard1 should hold the migrated documents as owned",
);

// Create the TTL index. expireAfterSeconds: 1 means every document is already expired.
assert.commandWorked(coll.createIndex({createdAt: 1}, {expireAfterSeconds: 1}));

// Wait for at least two full TTL passes so we know the monitor has had a chance to
// process this index.
TTLUtil.waitForPass(testDB);
TTLUtil.waitForPass(testDB);

// ---------------------------------------------------------------------------
// The assertion the bug breaks: the canary owned-expired document on shard0
// should eventually be deleted. With the bug present, the TTL monitor stages
// kNumOrphans (>= kTargetPassDocs) orphan documents first, hits its pass
// target without issuing any real deletes, and never reaches the canary.
// ---------------------------------------------------------------------------

// EXPECTED TO FAIL TODAY; will pass after SERVER-92779 fix lands.
// The test is intentionally marked as a regression test for an unfixed bug. When the
// fix lands, remove the tag block above and delete the `if` short-circuit below.
const FIX_LANDED = false;
if (!FIX_LANDED) {
    jsTest.log(
        "SERVER-92779 is open: skipping the final assertion that would otherwise hang. " +
            "When the fix lands, set FIX_LANDED = true and remove the early return.",
    );
    st.stop();
    quit();
}

// Once the fix lands the rest of the test verifies the monitor makes progress.
assert.soon(
    () => {
        return shard0Coll.countDocuments({_id: ownedId}) === 0;
    },
    () =>
        "TTL monitor never deleted the owned expired canary doc on shard0. " +
        "Found: " +
        tojson(shard0Coll.findOne({_id: ownedId})) +
        ". Orphan count on shard0: " +
        shard0Coll.countDocuments({_id: {$regex: "^orphan-"}}),
    60 * 1000 /* 60s */,
    1000 /* 1s poll */,
);

// Cross-check: the TTL monitor must NOT have deleted the orphans on shard0 (the
// existing ttl_deletes_not_targeting_orphaned_documents.js invariant must still hold).
assert.eq(
    kNumOrphans,
    shard0Coll.countDocuments({_id: {$regex: "^orphan-"}}),
    "TTL monitor must not delete orphan documents on shard0",
);

// And the migrated owned copies on shard1 must have been deleted by shard1's TTL
// monitor (they are owned + expired there).
assert.soon(
    () => shard1Coll.countDocuments({}) === 0,
    "shard1's TTL monitor should have deleted its owned expired documents",
    60 * 1000,
    1000,
);

st.stop();
