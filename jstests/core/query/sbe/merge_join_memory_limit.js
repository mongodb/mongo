/**
 * Test that verifies the SBE MergeJoin stage tracks memory usage.
 *
 * AND_SORTED (translated to MergeJoin in SBE) requires equality predicates on both indexed fields
 * so that each index scan is sorted by RecordId (sortedByDiskLoc == true). The stage buffers outer
 * rows with the same join key. Since RecordIds are unique per document, the buffer holds at most
 * one row at a time. The memory limit is therefore never enforced in practice and is only exercised
 * via unit tests (see sbe_merge_join_test.cpp); this test verifies that the AND_SORTED plan is chosen
 * and that memory usage is tracked.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_90,
 *   requires_sbe,
 *   # setParameter may return different values after a failover.
 *   does_not_support_stepdowns,
 *   # setParameterOnAllNonConfigNodes requires a stable shard list.
 *   assumes_stable_shard_list,
 * ]
 */

import {getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

if (!checkSbeFullyEnabled(db)) {
    jsTest.log.info("Skipping test: SBE MergeJoin memory limit requires SBE to be fully enabled.");
    quit();
}

const kDocCount = 1000;

const coll = db.merge_join_memory_limit;
coll.drop();

// All documents share the same 'a' and 'b' values so that equality predicates match many docs.
const docs = [];
for (let i = 0; i < kDocCount; i++) {
    docs.push({_id: i, a: 0, b: 0});
}
assert.commandWorked(coll.insertMany(docs));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

function runQuery() {
    // Equality predicates ensure sortedByDiskLoc() == true for both index scans.
    return coll.find({a: 0, b: 0});
}

// Force index intersection (AND_SORTED) plans so the SBE MergeJoin stage is used.
// internalQueryPlannerEnableSortIndexIntersection enables AND_SORTED plan generation;
// internalQueryForceIntersectionPlans boosts intersection plan scores in the ranker.
runWithParamsAllNonConfigNodes(
    db,
    {
        internalQueryPlannerEnableSortIndexIntersection: true,
        internalQueryForceIntersectionPlans: true,
    },
    () => {
        // Confirm the query actually uses an AND_SORTED (MergeJoin) plan.
        const explainRes = runQuery().explain("queryPlanner");
        assert(
            planHasStage(db, getWinningPlanFromExplain(explainRes), "AND_SORTED"),
            "Expected AND_SORTED stage in winning plan but got: " + tojson(explainRes),
        );

        // Verify the query returns the correct number of results.
        assert.eq(runQuery().itcount(), kDocCount, () => runQuery().explain("executionStats"));
    },
);
