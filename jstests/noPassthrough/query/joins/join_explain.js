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
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

let conn = MongoRunner.runMongod({
    setParameter: {featureFlagPathArrayness: true, featureFlagPersistentStats: true},
});

const db = conn.getDB("test");

// TODO SERVER-135439: Remove once featureFlagPersistentStats is enabled by default.
if (!FeatureFlagUtil.isEnabled(db, "PersistentStats")) {
    jsTest.log.info(`Skipping ${jsTestName()}: featureFlagPersistentStats is not enabled`);
    MongoRunner.stopMongod(conn);
    quit();
}

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
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll1.createIndex({dummy: 1, a: 1, b: 1, c: 1, d: 1}));
assert.commandWorked(coll2.createIndex({dummy: 1, a: 1, b: 1, c: 1, d: 1}));
assert.commandWorked(coll3.createIndex({dummy: 1, a: 1, b: 1, c: 1, d: 1}));

function validateJoinCostComponents(
    explain,
    expectedCardinalityRHSBeforeJoinPred,
    expectedNdvSource,
) {
    const winningStages = getAllPlanStages(getWinningPlanFromExplain(explain));
    const joinStages = winningStages.filter(plannerStageIsJoinOptNode);
    assert.gt(joinStages.length, 0, "Expected join opt stages", {explain});

    for (const stage of joinStages) {
        assert(
            stage.hasOwnProperty("joinCostComponents"),
            "joinCostComponents missing from join opt stage",
            {stage},
        );
        const c = stage.joinCostComponents;

        // All join types expose these numeric fields.
        for (const field of [
            "docsProcessed",
            "docsOutput",
            "numDocsTransmitted",
            "sequentialIOPages",
            "randomIOPages",
            "localOpCost",
            "totalCost",
        ]) {
            assert(c.hasOwnProperty(field), field + " missing from joinCostComponents", {field, c});
            assert.gte(c[field], 0, field + " must be non-negative", {field, c});
        }

        // The breakdown's totalCost duplicates the costEstimate.
        assert.eq(c.totalCost, stage.costEstimate, "totalCost does not match costEstimate", {
            stage,
        });

        // Each join embedding node reports the NDV estimate(s) and their provenance behind the
        // join edge(s) it applies.
        assert(stage.hasOwnProperty("ndvEstimates"), "ndvEstimates missing from join stage", {
            stage,
        });
        assert.gt(stage.ndvEstimates.length, 0, "Expected at least one ndv estimate", {
            stage,
        });
        const childNamespaces = [coll1.getFullName(), coll2.getFullName(), coll3.getFullName()];
        for (const est of stage.ndvEstimates) {
            assert.gte(est.ndv, 0, "ndv must be non-negative", {est});
            assert.eq(est.ndvSource, expectedNdvSource, "Unexpected ndvSource", est);
            assert(
                childNamespaces.includes(est.assumedPkSide),
                "Expected primary key to be chosen from available namespaces",
                {stage, childNamespaces, assumedPkSide: est.assumedPkSide},
            );
        }

        if (stage.stage == "INDEXED_NESTED_LOOP_JOIN_EMBEDDING") {
            const validMackertLohmanCases = new Set([
                "collection-fits-cache",
                "returned-docs-fit-cache",
                "partial-eviction",
            ]);
            assert(
                c.hasOwnProperty("mackertLohmanCase"),
                "mackertLohmanCase missing from INLJ costComponents",
                {c},
            );
            assert(
                validMackertLohmanCases.has(c.mackertLohmanCase),
                "Unexpected mackertLohmanCase value",
                {c},
            );

            assert.eq(
                expectedCardinalityRHSBeforeJoinPred,
                stage.cardinalityRHSBeforeJoinPred,
                "cardinalityRHSBeforeJoinPred must be the RHS cardinality after its own " +
                    "predicates",
                {stage},
            );
        } else {
            assert(
                !c.hasOwnProperty("mackertLohmanCase"),
                "Unexpected mackertLohmanCase outside INLJ node",
                {stage},
            );
        }
    }
}

// Runs the pipeline, and asserts that the join optimizer was used and that estimate information is
// present in the explain output.
function runTest(pipeline, expectRejected) {
    // Ensure stable on-disk sizes.
    assert.commandWorked(db.adminCommand({fsync: 1}));
    const explain = coll1.explain().aggregate(pipeline);

    const queryPlanner = getQueryPlanner(explain);
    const usedJoinOptimization = queryPlanner.winningPlan.hasOwnProperty("usedJoinOptimization")
        ? queryPlanner.winningPlan.usedJoinOptimization
        : false;
    assert(usedJoinOptimization, "Join optimizer was not used as expected", {explain});

    const joinMetrics = queryPlanner.joinOptimizationMetrics;
    assert(joinMetrics, "joinOptimizationMetrics missing from queryPlanner", {queryPlanner});

    // The outer phase timers are always populated when join optimization ran.
    for (const field of ["joinModelingTimeMicros", "sbeLoweringTimeMicros"]) {
        assert(joinMetrics.hasOwnProperty(field), field + " missing from joinOptimizationMetrics", {
            field,
            joinMetrics,
        });
        assert.gte(joinMetrics[field], 0, field + " must be >= 0", {field, joinMetrics});
    }

    // The plan enumeration metrics are present because explain never consults the join plan cache,
    // so join optimization always runs plan enumeration on an explain.
    for (const field of [
        "samplingTimeMicros",
        "cbrPlanningTimeMicros",
        "planEnumerationTimeMicros",
        "ceTimeMicros",
        "wtLeafPagesAvailable",
    ]) {
        assert(joinMetrics.hasOwnProperty(field), field + " missing from joinOptimizationMetrics", {
            field,
            joinMetrics,
        });
    }
    assert.eq("boolean", typeof joinMetrics.wtLeafPagesAvailable, joinMetrics);

    jsTest.log.info("Explain output", {explain});
    const winningPlan = getWinningPlanFromExplain(explain);
    const winningCost = winningPlan.costEstimate;
    const stages = getAllPlanStages(winningPlan);
    assert(
        stages.some((stage) => stage.stage.includes("JOIN_EMBEDDING")),
        "Expecting JOIN_EMBEDDING stage",
        {explain},
    );

    for (const stage of stages) {
        // TODO SERVER-111913: Once we have estimates from single table nodes, we can extend this to
        // check other kinds of stages and test that numDocs/keys are reported in the output.
        if (stage.stage.includes("JOIN_EMBEDDING") || stage.stage.includes("COLLSCAN")) {
            assert(
                stage.hasOwnProperty("cardinalityEstimate"),
                "Cardinality estimate not found in stage",
                {stage, explain},
            );
            assert.gt(stage.cardinalityEstimate, 0, "Cardinality estimate is not greater than 0");
            assert(stage.hasOwnProperty("costEstimate"), "Cost estimate not found in stage", {
                stage,
                explain,
            });
            assert.gt(stage.costEstimate, 0, "Cost estimate is not greater than 0");
        }
    }

    const rejectedPlans = getRejectedPlans(explain);
    if (expectRejected) {
        assert.gt(rejectedPlans.length, 0);
    }
    for (const plan of rejectedPlans) {
        // All plans should use join optimization.
        assert(plan.usedJoinOptimization, "Join optimizer was not used as expected", {
            plan,
            explain,
        });

        // Should have CE.
        assert(
            plan.queryPlan.hasOwnProperty("cardinalityEstimate"),
            "Cardinality estimate not found in rejected plan",
            {plan, explain},
        );
        assert.gt(
            plan.queryPlan.cardinalityEstimate,
            0,
            "Cardinality estimate is not greater than 0",
        );

        // Should have cost.
        assert(
            plan.queryPlan.hasOwnProperty("costEstimate"),
            "Cost estimate not found in rejected plan",
            {plan, explain},
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
    // Verify that when the knob is OFF, joinCostComponents is absent (default behaviour).
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalQueryExplainJoinCostComponents: false}),
    );
    {
        const explain = coll1.explain().aggregate(pipeline);
        const joinStages = getAllPlanStages(getWinningPlanFromExplain(explain)).filter(
            plannerStageIsJoinOptNode,
        );
        assert.gt(joinStages.length, 0, "Expected join opt stages", {explain});
        for (const stage of joinStages) {
            assert(
                !stage.hasOwnProperty("joinCostComponents"),
                "joinCostComponents should be absent when knob is off",
                {stage},
            );
            assert(
                !stage.hasOwnProperty("ndvEstimates"),
                "ndvEstimates should be absent when knob is off",
                {stage},
            );
        }

        // We expect only non-join cost estimates to be present.
        const estimatedStages = getAllPlanStages(getWinningPlanFromExplain(explain)).filter(
            (stage) => stage.hasOwnProperty("costEstimate"),
        );
        assert.gt(estimatedStages.length, 0, "Expected estimated stages", {explain});
        for (const stage of estimatedStages) {
            assert(
                !stage.hasOwnProperty("joinCostComponents"),
                "joinCostComponents should be absent when knob is off",
                {stage},
            );
        }
    }

    // Verify that when the knob is ON, joinCostComponents and ndvEstimates are present on every
    // join opt stage. Enable the use of unique indexes for NDV estimation.
    assert.commandWorked(
        conn.adminCommand({
            setParameter: 1,
            internalQueryExplainJoinCostComponents: true,
            internalEnableJoinOptimizationUseIndexUniqueness: true,
            internalQueryEnablePersistentNDVStats: true,
        }),
    );
    {
        // Basic case. Every RHS filter in 'pipeline' matches all 100 documents, so each INLJ stage must report exactly the RHS collection size for 'cardinalityRHSBeforeJoinPred'.
        const explain = coll1.explain().aggregate(pipeline);
        validateJoinCostComponents(
            explain,
            100 /* expectedCardinalityRHSBeforeJoinPred */,
            "sampling" /* expectedNdvSource */,
        );
    }

    // Force INLJ so that every join stage is an INLJ and mackertLohmanCase is always present.
    const inljHint = {
        perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", hint: {method: "INLJ"}}],
    };
    const inljPipeline = [{$_internalJoinHint: inljHint}].concat(pipeline);
    {
        // Repeat with all INLJ nodes.
        const explain = coll1.explain().aggregate(inljPipeline);
        validateJoinCostComponents(
            explain,
            100 /* expectedCardinalityRHSBeforeJoinPred */,
            "sampling" /* expectedNdvSource */,
        );

        // Verify the semantics of 'cardinalityRHSBeforeJoinPred' with a selective RHS predicate:
        // it must report the number of RHS docs matching the RHS's own predicates (50 of 100) and
        // not the whole RHS collection size.
        {
            const selectivePipeline = [{$_internalJoinHint: inljHint}].concat([
                {
                    $lookup: {
                        from: coll2.getName(),
                        localField: "a",
                        foreignField: "b",
                        as: "coll2",
                        pipeline: [{$match: {d: {$gte: 50}}}],
                    },
                },
                {$unwind: "$coll2"},
            ]);
            const explain = coll1.explain().aggregate(selectivePipeline);
            // Now we have a filter, so 'cardinalityRHSBeforeJoinPred' reports how many docs match the filter (50).
            validateJoinCostComponents(
                explain,
                50 /* expectedCardinalityRHSBeforeJoinPred */,
                "sampling" /* expectedNdvSource */,
            );
        }
    }

    {
        // Now validate the HLL case.
        assert.commandWorked(db.runCommand({analyze: coll1.getName(), mode: "ndv", key: "b"}));
        assert.commandWorked(db.runCommand({analyze: coll1.getName(), mode: "ndv", key: "c"}));
        const explain = coll1.explain().aggregate(inljPipeline);
        validateJoinCostComponents(
            explain,
            100 /* expectedCardinalityRHSBeforeJoinPred */,
            // TODO SERVER-135494: this should be hyperLogLog.
            "sampling" /* expectedNdvSource */,
        );
    }

    {
        // Now validate the unique index source case. If available, it is prioritized over HLL.
        assert.commandWorked(coll1.createIndex({b: 1}, {unique: true}));
        assert.commandWorked(coll1.createIndex({c: 1}, {unique: true}));
        const explain = coll1.explain().aggregate(inljPipeline);
        validateJoinCostComponents(
            explain,
            100 /* expectedCardinalityRHSBeforeJoinPred */,
            "uniqueIndex" /* expectedNdvSource */,
        );
    }

    // Reset the knob to its default.
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalQueryExplainJoinCostComponents: false}),
    );
}

MongoRunner.stopMongod(conn);
