/**
 * Ensure that the SBE planCache do not use the stale cache entries after
 * the refineCollectionShardKey operation.
 *
 * @tags: [
 *   # This test is specifically verifying the behavior of the SBE plan cache, which is only enabled
 *   # when SBE is enabled.
 *   featureFlagSbeFull
 * ]
 */

import {
    getPlanCacheKeyFromPipeline,
    getPlanCacheShapeHashFromObject
} from "jstests/libs/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "testdb";
const collName = "sbe_plan_cache_refined_shard_key";
const ns = `${dbName}.${collName}`;

const st = new ShardingTest({shard: 1, mongos: 1, config: 1});
const coll = st.getDB(dbName)[collName];
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Runs a test case against 'coll' with the given 'documents' and 'indexes'. The test will run the
// aggregation 'pipeline' first and check if the SBE plan is stored in the SBE plan cache, it will
// call the refineCollectionShardKey function to change the shard key from ' shardKeyBefore' to
// 'shardKeyAfter' and check if the corresponding planCacheKey has changed.
function runTest({documents, indexes, pipeline, shardKeyBefore, shardKeyAfter}) {
    coll.drop();
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKeyBefore}));

    assert.commandWorked(coll.insertMany(documents));
    assert.commandWorked(coll.createIndexes(indexes));

    const planCacheKeyBefore = getPlanCacheKeyFromPipeline(pipeline, coll);

    coll.aggregate(pipeline);
    const planCacheStatsBefore = coll.aggregate([{$planCacheStats: {}}]).toArray();
    const planCacheEntryBefore =
        planCacheStatsBefore.find(entry => entry["planCacheKey"] === planCacheKeyBefore);
    assert.eq(planCacheEntryBefore["version"], 2, "before entry should be cached as SBE plan");

    st.adminCommand({refineCollectionShardKey: ns, key: shardKeyAfter});

    const planCacheKeyAfter = getPlanCacheKeyFromPipeline(pipeline, coll);

    coll.aggregate(pipeline);
    const planCacheStatsAfter = coll.aggregate([{$planCacheStats: {}}]).toArray();
    const planCacheEntryAfter =
        planCacheStatsAfter.find(entry => entry["planCacheKey"] === planCacheKeyAfter);
    assert.eq(planCacheEntryAfter["version"], 2, "after entry should be cached as SBE plan");

    // After calling "refineCollectionShardKey", the queries should have the same
    // 'planCacheShapeHash' but different 'planCacheKey'.
    assert.eq(getPlanCacheShapeHashFromObject(planCacheEntryBefore),
              getPlanCacheShapeHashFromObject(planCacheEntryAfter),
              "plan cache shape hash should be the same after refining");
    assert.neq(
        planCacheKeyBefore, planCacheKeyAfter, "plan cache key should be different after refining");
}

runTest({
    documents: [{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 1}, {a: 2, b: 2}],
    indexes: [{a: 1}, {b: 1}, {a: 1, b: 1}],
    pipeline: [{$match: {a: 1}}, {$group: {_id: "$b", sum: {$sum: "$a"}}}],
    shardKeyBefore: {a: 1},
    shardKeyAfter: {a: 1, b: 1}
});

st.stop();
