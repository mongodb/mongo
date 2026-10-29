/**
 * Test that for SBE plans a plan cache entry includes a serialized SBE plan tree, and does not for
 * classic plans.
 *
 * @tags: [
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   # This test attempts to perform queries with plan cache filters set up. The former operation
 *   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
 *   # primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   # This test checks a new field "solutionHash" in $planCacheStats, not available in previous
 *   # versions.
 *   requires_fcv_72,
 *   multiversion_incompatible,
 *   # Checks that SBE is never used when SBE full is not enabled. For implicitly created column
 *   # indexes this check would be violated, since it is not covered by other SBE feature flags.
 *   assumes_no_implicit_index_creation,
 *   # This test looks for plan cache hits, which would change with repeated reads.
 *   does_not_support_repeated_reads,
 *   # The test examines the SBE plan cache, which initial sync may change the contents of.
 *   examines_sbe_cache,
 *   requires_getmore,
 *   # Not first stage in pipeline. The following test uses $planCacheStats, which is required to be the
 *   # first stage in a pipeline. This will be incomplatible with timeseries.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const coll = db.plan_cache_sbe;
coll.drop();

const shouldGenerateSbePlan = checkSbeFullyEnabled(db);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({a: i, b: 1}));
}

// We need two indexes so that the multi-planner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: -1, b: 1}));

assert.eq(1, coll.find({a: 1}).itcount());

// Validate plan cache stats entry.
const allStats = coll.aggregate([{$planCacheStats: {}}]).toArray();
assert.eq(allStats.length, 1, allStats);
const stats = allStats[0];
assert(stats.hasOwnProperty("cachedPlan"), stats);
assert(stats.hasOwnProperty("solutionHash"), stats);

assert(!stats.cachedPlan.hasOwnProperty("queryPlan"), stats);
assert(!stats.cachedPlan.hasOwnProperty("slotBasedPlan"), stats);

if (shouldGenerateSbePlan) {
    // Test that a plan whose match expression has > 512 predicates does not get cached for SBE,
    // because in that case it will not be auto-parameterized, so caching the plan would cause
    // cache flooding with plans that will likely never be resused (SERVER-79867). Also verify
    // the results are correct.
    const kDocFields = 513;
    let doc0 = {_id: 0};
    let doc1 = {_id: 1};
    for (let field = 0; field < kDocFields; ++field) {
        doc0["field_" + field] = 0;
        doc1["field_" + field] = 1;
    }
    doc0 = sortDoc(doc0);
    doc1 = sortDoc(doc1);
    assert.commandWorked(coll.insert(doc0));
    assert.commandWorked(coll.insert(doc1));

    // Define $match stages with kDocFields > 512 match predicates.
    let match0 = {"$match": doc0};
    let match1 = {"$match": doc1};
    coll.getPlanCache().clear();

    // Run each query twice as query plans do not show up in cache stats until they have been
    // activated, which occurs on the second execution.
    for (let i = 0; i < 2; ++i) {
        let agg = coll.aggregate([match0]).toArray();
        assert.eq(1, agg.length);
        assert.eq(doc0, sortDoc(agg[0]), sortDoc(agg[0]));
    }

    for (let i = 0; i < 2; ++i) {
        let agg = coll.aggregate([match1]).toArray();
        assert.eq(1, agg.length);
        assert.eq(doc1, sortDoc(agg[0]), sortDoc(agg[0]));
    }

    // There should be zero SBE plan cache entries.
    const planCacheStats = coll.aggregate([{$planCacheStats: {}}]).toArray();
    assert.eq(0, planCacheStats.length, planCacheStats);
}
