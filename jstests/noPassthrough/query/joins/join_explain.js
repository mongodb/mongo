/**
 * Ensures that the join optimizer populates estimate information (e.g., CE) in the explain output.
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */

import {
    getWinningPlanFromExplain,
    getAllPlanStages,
    getQueryPlanner,
    getRejectedPlans,
} from "jstests/libs/query/analyze_plan.js";
import {plannerStageIsJoinOptNode} from "jstests/libs/query/join_utils.js";

let conn = MongoRunner.runMongod();

const db = conn.getDB("test");

const coll1 = db[jsTestName()];
const coll2 = db[jsTestName() + "_2"];
const coll3 = db[jsTestName() + "_3"];

coll1.drop();
coll2.drop();
coll3.drop();

let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({_id: i, a: i, b: i, c: i, d: i});
}
assert.commandWorked(coll1.insertMany(docs));
assert.commandWorked(coll2.insertMany(docs));
assert.commandWorked(coll3.insertMany(docs));
assert.commandWorked(coll3.createIndex({d: 1}));

// Runs the pipeline, and asserts that the join optimizer was used and that estimate information is
// present in the explain output.
function runTest(pipeline, expectRejected) {
    const explain = coll1.explain().aggregate(pipeline);

    const queryPlanner = getQueryPlanner(explain);
    const usedJoinOptimization = queryPlanner.winningPlan.hasOwnProperty("usedJoinOptimization")
        ? queryPlanner.winningPlan.usedJoinOptimization
        : false;
    assert(usedJoinOptimization, "Join optimizer was not used as expected: " + tojson(explain));

    jsTest.log.info("Explain output: " + tojson(explain));
    const winningPlan = getWinningPlanFromExplain(explain);
    const winningCost = winningPlan.costEstimate;
    const stages = getAllPlanStages(winningPlan);
    assert(
        stages.some((stage) => stage.stage.includes("JOIN_EMBEDDING")),
        "Expecting JOIN_EMBEDDING stage in: " + tojson(explain),
    );

    for (const stage of stages) {
        // TODO SERVER-111913: Once we have estimates from single table nodes, we can extend this to
        // check other kinds of stages and test that numDocs/keys are reported in the output.
        if (stage.stage.includes("JOIN_EMBEDDING") || stage.stage.includes("COLLSCAN")) {
            assert(
                stage.hasOwnProperty("cardinalityEstimate"),
                "Cardinality estimate not found in stage: " + tojson(stage) + ", " + tojson(explain),
            );
            assert.gt(stage.cardinalityEstimate, 0, "Cardinality estimate is not greater than 0");
            assert(
                stage.hasOwnProperty("costEstimate"),
                "Cost estimate not found in stage: " + tojson(stage) + ", " + tojson(explain),
            );
            assert.gt(stage.costEstimate, 0, "Cost estimate is not greater than 0");
        }
    }

    const rejectedPlans = getRejectedPlans(explain);
    if (expectRejected) {
        assert.gt(rejectedPlans.length, 0);
    }
    for (const plan of rejectedPlans) {
        // All plans should use join optimization.
        assert(plan.usedJoinOptimization, "Join optimizer was not used as expected: " + tojson(explain));

        // Should have CE.
        assert(
            plan.queryPlan.hasOwnProperty("cardinalityEstimate"),
            "Cardinality estimate not found in rejected plan: " + tojson(plan.queryPlan) + ", " + tojson(explain),
        );
        assert.gt(plan.queryPlan.cardinalityEstimate, 0, "Cardinality estimate is not greater than 0");

        // Should have cost.
        assert(
            plan.queryPlan.hasOwnProperty("costEstimate"),
            "Cost estimate not found in rejected plan: " + tojson(plan.queryPlan) + ", " + tojson(explain),
        );
        assert.gt(plan.queryPlan.costEstimate, 0, "Cost estimate is not greater than 0");

        // Should have a larger cost than the 'winning' plan.
        assert.gt(plan.queryPlan.costEstimate, winningCost, "Cost estimate <= winning plan cost!");
    }
}

assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

// This pipeline has three single-table predicates, where one collection has a supporting index and
// the other two do not.
const pipeline = [
    {$match: {d: {$gte: 0}}},
    {
        $lookup: {
            from: coll2.getName(),
            localField: "b",
            foreignField: "b",
            as: "coll2",
            pipeline: [{$match: {d: {$gte: 0}}}],
        },
    },
    {$unwind: "$coll2"},
    {
        $lookup: {
            from: coll3.getName(),
            localField: "c",
            foreignField: "c",
            as: "coll3",
            pipeline: [{$match: {d: {$gte: 0}}}],
        },
    },
    {$unwind: "$coll3"},
];

// No indexes.
runTest(pipeline, false /* expectRejected */);

// With indexes.
assert.commandWorked(coll2.createIndex({b: 1}));
assert.commandWorked(coll3.createIndex({c: 1}));
runTest(pipeline, true /* expectRejected */);

// Test joinCostComponents explain output (internalQueryExplainJoinCostComponents knob).
{
    const validMackertLohmanCases = new Set(["collection-fits-cache", "returned-docs-fit-cache", "partial-eviction"]);

    // Verify that when the knob is OFF, joinCostComponents is absent (default behaviour).
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalQueryExplainJoinCostComponents: false}));
    {
        const explain = coll1.explain().aggregate(pipeline);
        const joinStages = getAllPlanStages(getWinningPlanFromExplain(explain)).filter(plannerStageIsJoinOptNode);
        assert.gt(joinStages.length, 0, "Expected join opt stages: " + tojson(explain));
        for (const stage of joinStages) {
            assert(
                !stage.hasOwnProperty("joinCostComponents"),
                "joinCostComponents should be absent when knob is off: " + tojson(stage),
            );
        }
    }

    // Verify that when the knob is ON, joinCostComponents is present on every join opt stage.
    // Force INLJ so that every join stage is an INLJ and mackertLohmanCase is always present.
    assert.commandWorked(
        conn.adminCommand({
            setParameter: 1,
            internalQueryExplainJoinCostComponents: true,
            internalJoinMethod: "INLJ",
        }),
    );
    {
        const explain = coll1.explain().aggregate(pipeline);
        const joinStages = getAllPlanStages(getWinningPlanFromExplain(explain)).filter(plannerStageIsJoinOptNode);
        assert.gt(joinStages.length, 0, "Expected join opt stages: " + tojson(explain));

        for (const stage of joinStages) {
            assert(
                stage.hasOwnProperty("joinCostComponents"),
                "joinCostComponents missing from join opt stage: " + tojson(stage),
            );
            const c = stage.joinCostComponents;

            // All join types expose these numeric fields.
            for (const field of ["docsProcessed", "docsOutput", "sequentialIOPages", "randomIOPages", "localOpCost"]) {
                assert(c.hasOwnProperty(field), field + " missing from joinCostComponents: " + tojson(c));
                assert.gte(c[field], 0, field + " must be non-negative: " + tojson(c));
            }

            // All stages are INLJ, so mackertLohmanCase must always be present.
            assert(
                c.hasOwnProperty("mackertLohmanCase"),
                "mackertLohmanCase missing from INLJ costComponents: " + tojson(c),
            );
            assert(
                validMackertLohmanCases.has(c.mackertLohmanCase),
                "Unexpected mackertLohmanCase value '" + c.mackertLohmanCase + "': " + tojson(c),
            );
        }
    }

    // Reset the knobs to their defaults.
    assert.commandWorked(
        conn.adminCommand({
            setParameter: 1,
            internalQueryExplainJoinCostComponents: false,
            internalJoinMethod: "any",
        }),
    );
}

MongoRunner.stopMongod(conn);
