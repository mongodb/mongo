/**
 * Test that 'planCacheShapeHash' and 'planCacheKey' from explain() output have sensible values
 * across catalog changes.
 * @tags: [
 *   assumes_read_concern_local,
 *   # This test expects query shapes and plans to stay the same at the beginning and
 *   # at the end of test run. That's just wrong expectation when chunks are moving
 *   # randomly across shards.
 *   assumes_balancer_off,
 *   requires_fcv_51,
 *   # There are no guarantees about how 'planCacheShapeHash' is computed between different versions
 *   # of binaries. Therefore, this test can fail if a stepdown occurs between the two explains,
 *   # as it compares two different 'planCacheShapeHash' values.
 *   multiversion_incompatible,
 *   # The test expects the plan cache key on a given node to remain stable. However, the plan
 *   # cache key is allowed to change between versions. Therefore, this test cannot run in
 *   # passthroughs that do upgrade/downgrade.
 *   cannot_run_during_upgrade_downgrade,
 *   # This test expects stable query plans, creating unanticipated indexes can lead to variations
 *   # in the plans.
 *   assumes_no_implicit_index_creation,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    getAllNodeExplains,
    getPlanCacheKeyFromExplain,
    getPlanCacheShapeHashFromExplain
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

function groupBy(arr, keyFn) {
    let dict = {};
    for (const elem of arr) {
        const key = keyFn(elem);
        if (!dict.hasOwnProperty(key)) {
            dict[key] = [];
        }
        dict[key].push(elem);
    }
    return dict;
}

// SERVER-56980: When running in a sharded environment, we group the explains by shard. This is
// because in a multi-version environment, we want to ensure that we are comparing the results
// produced by the same shard in the event that the 'planCacheKey' format changed in between
// versions.
function runTest({explain0, explain1, assertionFn}) {
    const groupedExplains =
        groupBy([...getAllNodeExplains(explain0), ...getAllNodeExplains(explain1)],
                /* keyFn */ (explain) => explain.shardName);
    for (const group of Object.values(groupedExplains)) {
        assert.eq(group.length, 2);
        const [nodeExplain0, nodeExplain1] = group;
        assertionFn(nodeExplain0, nodeExplain1);
    }
}

const coll = assertDropAndRecreateCollection(db, "plan_cache_shape_hash_stability");
assert.commandWorked(coll.insert({x: 5}));

const query = {
    x: 3
};
const initialExplain = coll.find(query).explain();

// Add a sparse index.
assert.commandWorked(coll.createIndex({x: 1}, {sparse: true}));
const withIndexExplain = coll.find(query).explain();
runTest({
    explain0: initialExplain,
    explain1: withIndexExplain,
    assertionFn: (nodeExplain0, nodeExplain1) => {
        assert.eq(getPlanCacheShapeHashFromExplain(nodeExplain0),
                  getPlanCacheShapeHashFromExplain(nodeExplain1),
                  "'planCacheShapeHash' shouldn't change accross catalog changes");

        // We added an index so the plan cache key changed.
        assert.neq(getPlanCacheKeyFromExplain(nodeExplain0),
                   getPlanCacheKeyFromExplain(nodeExplain1),
                   "'planCacheKey' should change accross catalog changes");
    }
});

// Drop the index.
assert.commandWorked(coll.dropIndex({x: 1}));
const postDropExplain = coll.find(query).explain();
const usesSbePlanCache = checkSbeFullFeatureFlagEnabled(db);
runTest({
    explain0: initialExplain,
    explain1: postDropExplain,
    assertionFn: (nodeExplain0, nodeExplain1) => {
        assert.eq(getPlanCacheShapeHashFromExplain(nodeExplain0),
                  getPlanCacheShapeHashFromExplain(nodeExplain1),
                  "'planCacheShapeHash' shouldn't change accross catalog changes");

        if (usesSbePlanCache) {
            // SBE's 'planCacheKey' encoding encodes "collection version" which will be increased
            // after dropping an index.
            assert.neq(getPlanCacheKeyFromExplain(nodeExplain0),
                       getPlanCacheKeyFromExplain(nodeExplain1),
                       "'planCacheKey' should change accross catalog changes");
        } else {
            // The 'planCacheKey' should be the same as what it was before we dropped the index.
            assert.eq(getPlanCacheKeyFromExplain(nodeExplain0),
                      getPlanCacheKeyFromExplain(nodeExplain1));
        }
    }
});
