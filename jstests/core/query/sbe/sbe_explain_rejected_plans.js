/**
 * Tests that SBE reports correct rejected plans when calling explain().
 * @tags: [
 *    assumes_unsharded_collection,
 *    requires_fcv_63,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    getExecutionStages,
    getPlanStages,
    getRejectedPlan,
    getRejectedPlans,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const isSBEEnabled = checkSbeFullyEnabled(db);
if (!isSBEEnabled) {
    jsTestLog("Skipping test because SBE is disabled");
    quit();
}

const coll = assertDropAndRecreateCollection(db, "sbe_explain_rejected_plans");
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Insert data, such that index 'a_1_b_1' is preferred.
for (let a = 0; a < 10; a++) {
    for (let b = 0; b < 10; b++) {
        assert.commandWorked(coll.insert({a: a, b: b}));
    }
}

const a1b1IndexName = "a_1_b_1";
const a1IndexName = "a_1";
const b1IndexName = "b_1";
const explain = coll.find({a: 7, b: 9}).explain("executionStats");

let ixscans = getPlanStages(getWinningPlanFromExplain(explain), "IXSCAN");
assert.neq(ixscans.length, 0, explain);
for (let ixscan of ixscans) {
    assert.eq(ixscan.indexName, a1b1IndexName, {reason: "Expected a1b1 index to be used", explain});
}

// Verify that the winning SBE plan has index scan stage on 'a_1_b_1'.
const executionStages = getExecutionStages(explain);
assert.neq(executionStages.length, 0, {reason: "Expected explain stages to be present", explain});
for (let executionStage of executionStages) {
    let ixscans = getPlanStages(executionStage, "ixseek");
    assert.neq(ixscans.length, 0, {reason: "Expected ixseek to be present", explain, executionStages});
    for (let ixscan of ixscans) {
        assert.eq(ixscan.indexName, a1b1IndexName, "Expected a1b1 index to be used", {
            ixscan,
            ixscans,
            executionStage,
            explain,
        });
    }
}

// Verify that rejected plans should have index scan on 'a_1' or 'b_1'.
for (let rejectedPlan of getRejectedPlans(explain)) {
    let ixscans = getPlanStages(getRejectedPlan(rejectedPlan), "IXSCAN");
    assert.neq(ixscans.length, 0, {reason: "Expected rejected plans to use a1 or b1 index", explain});
    for (let ixscan of ixscans) {
        assert.contains(ixscan.indexName, [a1IndexName, b1IndexName], {
            reason: "Expected rejected plans to use a1 or b1 index",
            ixscan,
            ixscans,
            rejectedPlan,
            explain,
        });
    }
}
