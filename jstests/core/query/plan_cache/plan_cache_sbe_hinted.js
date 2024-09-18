/**
 * Test that queries with hints can be cached in the SBE plan cache.
 *
 * @tags: [
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   # The SBE plan cache for hinted query was introduced in 6.3.
 *   requires_fcv_63,
 *   # Plan cache state is node-local and will not get migrated alongside tenant data.
 *   tenant_migration_incompatible,
 *   # Multiple servers can mess up the plan cache list.
 *   assumes_standalone_mongod,
 *   featureFlagSbeFull,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkExperimentalCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";

const isBonsaiEnabled = checkExperimentalCascadesOptimizerEnabled(db);
const isBonsaiPlanCacheEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "OptimizerPlanCache");

const coll = db.plan_cache_sbe;
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.createIndexes([{a: 1}, {a: 1, b: 1}]));

const verifyPlanCacheSize = function(len) {
    const caches = coll.getPlanCache().list();
    assert.eq(len, caches.length, caches);
};
const queryAndVerify = function(hint, expected) {
    assert.eq(1, coll.find({a: 1}).hint(hint).itcount());
    assert.eq(1, coll.find({a: 1}).hint(hint).itcount());
    verifyPlanCacheSize(expected);
};
verifyPlanCacheSize(0);
if (!isBonsaiEnabled && !isBonsaiPlanCacheEnabled) {
    // TODO SERVER-85728: Re-enable index scans for Bonsai plan cache tests.
    // Hinted query is cached.
    queryAndVerify({a: 1}, 1);
    // Non-hinted query is cached as different entry.
    queryAndVerify({}, 2);
    // Hinted query cached is reused.
    queryAndVerify({a: 1}, 2);
    // Query with different hint.
    queryAndVerify({a: 1, b: 1}, 3);
}
