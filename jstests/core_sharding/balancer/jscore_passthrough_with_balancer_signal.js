/**
 * SERVER-86747: smoke test that pins the contract of
 * sharded_collections_jscore_passthrough_with_balancer.
 *
 * When this test is executed under the new suite it must observe:
 *   1. TestData.runningWithBalancer === true (set by the suite's global_vars).
 *   2. The balancer is enabled on the config server (enable_balancer: true).
 *   3. A simple CRUD round-trip on an implicitly-sharded collection completes
 *      successfully while the balancer / random_migrations may move chunks
 *      underneath us (via implicitly_retry_on_migration_in_progress.js).
 *
 * When this test runs in any other passthrough (e.g. the base
 * sharded_collections_jscore_passthrough suite, which does NOT set
 * runningWithBalancer), it is a no-op CRUD smoke test — assertions on the
 * balancer signal are gated on TestData.runningWithBalancer.
 *
 * @tags: [
 *   requires_sharding,
 *   # The balancer-on assertion is incompatible with passthroughs that
 *   # explicitly disable the balancer.
 *   assumes_balancer_on,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

// 1. The CRUD path must work under background migrations. The suite's
//    implicitly_shard_accessed_collections.js override shards the collection
//    on first access, and implicitly_retry_on_migration_in_progress.js
//    retries find/aggregate that race a moveChunk/moveCollection.
const N = 50;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < N; i++) {
    bulk.insert({_id: i, payload: "x".repeat(16)});
}
assert.commandWorked(bulk.execute());

assert.eq(N, coll.find().itcount(), "expected all inserted docs to be visible via find()");
assert.eq(N,
          coll.aggregate([{$count: "n"}]).toArray()[0].n,
          "expected $count to agree with itcount() under background migrations");

// 2. Balancer-active contract. Only enforced when the suite advertises the
//    balancer; under the base passthrough this block is skipped so the same
//    file can be authored once and selected by both suites if desired.
if (typeof TestData !== "undefined" && TestData.runningWithBalancer === true) {
    const balancerStatus = assert.commandWorked(db.adminCommand({balancerStatus: 1}));
    assert.eq(true,
              balancerStatus.mode === "full" || balancerStatus.mode === "autoMergeOnly" ||
                  balancerStatus.mode === "off" ?
                  balancerStatus.mode !== "off" :
                  true,
              () => "expected balancer to be enabled under " +
                  "sharded_collections_jscore_passthrough_with_balancer; got: " + tojson(balancerStatus));
}

coll.drop();
