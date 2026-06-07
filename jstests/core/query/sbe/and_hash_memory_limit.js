/**
 * Test that verifies the SBE AndHash stage fails when the memory limit is exceeded.
 *
 * AND_HASH is used for index intersection when the index scans are not sorted by RecordId
 * (i.e. range predicates). It eagerly builds a hash table of all outer-side records during
 * open(), so its memory grows proportionally to the number of matching documents — making it
 * straightforward to trigger the memory limit.
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
    jsTest.log.info("Skipping test: SBE AND_HASH memory limit requires SBE to be fully enabled.");
    quit();
}

const kDocCount = 1000;

const coll = db.and_hash_memory_limit;
coll.drop();

const docs = [];
for (let i = 0; i < kDocCount; i++) {
    docs.push({_id: i, a: i, b: i});
}
assert.commandWorked(coll.insertMany(docs));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

function runQuery() {
    return coll.find({a: {$gte: 0}, b: {$gte: 0}});
}

// Force index intersection (AND_HASH) plans. Range predicates mean sortedByDiskLoc() == false,
// so the planner picks AND_HASH rather than AND_SORTED.
// internalQueryPlannerEnableHashIntersection enables AND_HASH plan generation;
// internalQueryForceIntersectionPlans boosts intersection plan scores in the ranker.
runWithParamsAllNonConfigNodes(
    db,
    {
        internalQueryPlannerEnableHashIntersection: true,
        internalQueryForceIntersectionPlans: true,
    },
    () => {
        // Confirm the query actually uses an AND_HASH plan before proceeding.
        const explainRes = runQuery().explain("queryPlanner");
        assert(
            planHasStage(db, getWinningPlanFromExplain(explainRes), "AND_HASH"),
            "Expected AND_HASH stage in winning plan but got: " + tojson(explainRes),
        );

        // Verify the query succeeds with the default memory limit.
        assert.eq(runQuery().itcount(), kDocCount, () => runQuery().explain("executionStats"));

        // Set a 1-byte limit so the first document inserted into the hash table exceeds it.
        runWithParamsAllNonConfigNodes(db, {internalSlotBasedExecutionAndHashStageMaxMemoryBytes: 1}, () => {
            assert.throwsWithCode(
                () => runQuery().itcount(),
                12321801,
                [] /*params*/,
                () => runQuery().explain("executionStats"),
            );
        });
    },
);
