/**
 * Confirm that a hashed index field does not prevent the index prefix field to be used for covered
 * projection and to produce correct result.
 * @tags: [
 *      # Explain may return incomplete results if interrupted by a stepdown.
 *      does_not_support_stepdowns,
 * ]
 */

import {
    orderedArrayEq,
} from "jstests/aggregation/extras/utils.js";
import {
    getPlanStage,
    getShardsFromExplain,
    getWinningPlanFromExplain,
} from "jstests/libs/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    coll.insertMany([{_id: 1, a: {b: 5}}, {_id: 2, a: {b: 2, c: 1}}, {_id: 3, a: {b: 0}}]));

// Confirm that the hashed index scan produces the same results as the collection scan.
const resultsCollScan = coll.find({}, {_id: 0, 'a.b': 1}).sort({'a.b': 1});
assert.commandWorked(coll.createIndex({'a.b': 1, a: 'hashed'}));
const resultsIndexScan = coll.find({}, {_id: 0, 'a.b': 1}).sort({'a.b': 1});
assert(orderedArrayEq(resultsCollScan, resultsIndexScan));

// Check that the index with hashed field is used in the plan.
const explain = coll.find({}, {_id: 0, 'a.b': 1}).sort({'a.b': 1}).explain();

const plan = getWinningPlanFromExplain(explain);
const project = getPlanStage(plan, "PROJECTION_DEFAULT");
assert.neq(project, null, explain);
const ixScan = getPlanStage(plan, "IXSCAN");
assert.eq(ixScan.indexName, "a.b_1_a_hashed", explain);

const fetch = getPlanStage(plan, "FETCH");
const shards = getShardsFromExplain(explain);
if (shards) {
    // In sharded environment if a sharding_filter stage is added, a FETCH stage is also added on
    // top of the index scan. Otherwise covered projection is used without a fetch.
    const shardingFilter = getPlanStage(plan, "SHARDING_FILTER");
    if (shardingFilter) {
        assert.neq(fetch, null, plan);
    } else {
        assert.eq(fetch, null, plan);
    }
} else {
    // In non-sharded environment covered projection is used without a FETCH stage.
    assert.eq(fetch, null, plan);
}
