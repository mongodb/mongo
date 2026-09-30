/**
 * Tests the V3 "queryPlanner.plans" array. It runs the same query under every plan-ranker mode
 * (pure multi-planning, strict CBR with sampling and heuristic CE, and the default mixed mode
 * with both of its outcomes), plus the special plan cases (cached plan, single plan,
 * subplanned $or, EOF, count, express). Unless a case says otherwise the verbosity is
 * plannerStats; plannerChoice, which renders the same shape with every statistic withheld, has its
 * own case. For each resulting explain it asserts:
 *
 * - The per-plan object layout: {isCached, solutionHashUnstable, multiPlanEstimateStats,
 *   multiPlanFinalizeStats, planStages}.
 * - The per-node "statistics" grouping: {costBased, multiPlanEstimate, multiPlanFinalize}. The
 *   grouping is sparse - a group is present iff the corresponding statistic was computed for that
 *   node, and the "statistics" wrapper is absent when all groups are absent.
 * - The multi-planner groups are split per trial phase: a capped (estimate) phase emits
 *   multiPlanEstimate(Stats), the deciding uncapped phase multiPlanFinalize(Stats), each present
 *   iff the plan did work in that phase; the finalize counters are increments over the estimate
 *   phase, and 'score' and the plan's final 'stopCondition' are emitted inside the last present
 *   group.
 * - The ordering of the plans after the winner: by the deciding ranker's metric (trial score
 *   descending when the multi-planner decided, cost ascending when the cost-based ranker did).
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertChosenRanker,
    assertStopCondition,
    getV3Plans,
    MultiPlannerStopCondition,
    ChosenRanker,
    normalizeRunVarying,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";
import {
    assertAscending,
    assertDescending,
    assertWellFormedPlan,
    forEachNode,
    hasCostBasedGroup,
    hasMultiPlanGroup,
    hasNodeGroup,
    kPlanPhaseGroups,
    rootCostEstimate,
} from "jstests/libs/query/explain_v3_helpers.js";

const collName = jsTestName();
const coll = db[collName];

function explainFind(filter, verbosity = "plannerStats") {
    return assert.commandWorked(
        db.runCommand({explain: {find: collName, filter: filter}, verbosity}),
    );
}

// A filter that multi-plans across the a_1 and b_1 indexes and returns rows during the trial.
const matchingFilter = {a: {$gte: 0}, b: {$gte: 0}};
// Multi-plans across the same two indexes, but the residual filter on the unindexed 'c' rejects
// every document: the capped mixed-mode trial produces no results and - because the index scans
// yield far more keys than the per-plan works budget - does not reach EOF either, so the
// cost-based ranker decides. (A predicate with empty index bounds would instead EOF instantly,
// earning the EOF bonus and letting the multi-planner decide.)
const cbrWinFilter = {a: {$gte: 0}, b: {$gte: 0}, c: 1};

describe("V3 queryPlanner.plans array with classic engine", function () {
    let savedRankerConfig;
    let savedFrameworkControl;

    before(function () {
        savedRankerConfig = getPlanRankerConfig(db);
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );

        coll.drop();
        // Large enough that a capped mixed-mode trial over the full-range index scans can
        // neither produce a result (see cbrWinFilter) nor reach EOF within its works budget.
        const docs = [];
        for (let i = 0; i < 10000; ++i) {
            docs.push({_id: i, a: i % 100, b: i % 10});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
        // A third index so the multi-plan scenarios rank at least three candidates.
        assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    });

    after(function () {
        setPlanRankerConfig(db, savedRankerConfig);
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: savedFrameworkControl,
            }),
        );
    });

    it("pure multi-planning: finalize-only trial groups and score-descending order", function () {
        setPlanRankerConfig(db, {internalQueryPlanRanker: "multiPlanning"});
        const explain = explainFind(matchingFilter);
        assertChosenRanker(
            explain,
            ChosenRanker.kMultiPlanning,
            PlanRankerReason.kQueryPlanRankerKnob,
        );
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        const scores = [];
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            // Every candidate ran one uncapped trial: the classic trial is the deciding trial, so
            // only the finalize phase exists - node-level multiPlanFinalize groups and plan-level
            // multiPlanFinalizeStats with a score, and no estimate groups anywhere. The
            // cost-based ranker never ran, so no costBased either.
            assert(hasNodeGroup(plan, "multiPlanFinalize"), "expected multiPlanFinalize group", {
                plan,
            });
            assert(!hasNodeGroup(plan, "multiPlanEstimate"), "unexpected multiPlanEstimate group", {
                plan,
            });
            assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan});
            assert(plan.multiPlanFinalizeStats, "expected plan-level multiPlanFinalizeStats", {
                plan,
            });
            assert(
                !plan.hasOwnProperty("multiPlanEstimateStats"),
                "unexpected plan-level multiPlanEstimateStats",
                {plan},
            );
            assert(plan.multiPlanFinalizeStats.hasOwnProperty("score"), "expected trial score", {
                plan,
            });
            scores.push(plan.multiPlanFinalizeStats.score);
        }
        // The plans after the winner are in score-descending order (the multi-planner decided).
        assertDescending(scores.slice(1), plans);
    });

    it("strict CBR (sampling): costBased groups and cost-ascending order", function () {
        setPlanRankerConfig(db, {internalQueryPlanRanker: "costBased"});
        const explain = explainFind(matchingFilter);
        assertChosenRanker(explain, ChosenRanker.kCostBased, PlanRankerReason.kQueryPlanRankerKnob);
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        const costs = [];
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(hasCostBasedGroup(plan), "expected costBased node group", {plan});
            assert.eq(plan.planStages.statistics.costBased.estimatesMetadata.ceSource, "Sampling", {
                plan,
            });
            costs.push(rootCostEstimate(plan));
        }
        // CBR-rejected plans never ran a trial.
        for (const plan of plans.slice(1)) {
            assert(!hasMultiPlanGroup(plan), "unexpected multi-planner group", {plan});
            for (const group of kPlanPhaseGroups) {
                assert(!plan.hasOwnProperty(group), "unexpected plan-level phase group", {
                    plan,
                    group,
                });
            }
        }
        // The plans after the winner are in cost-ascending order (the cost-based ranker decided).
        assertAscending(costs.slice(1), plans);
    });

    it("strict CBR (heuristic): ceSource Heuristics", function () {
        setPlanRankerConfig(db, {
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "heuristicCE",
        });
        const explain = explainFind(matchingFilter);
        assertChosenRanker(explain, ChosenRanker.kCostBased, PlanRankerReason.kQueryPlanRankerKnob);
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(hasCostBasedGroup(plan), "expected costBased node group", {plan});
            assert.eq(
                plan.planStages.statistics.costBased.estimatesMetadata.ceSource,
                "Heuristics",
                {plan},
            );
        }
    });

    it("mixed (default), multi-planner wins: estimate-only groups carry the score", function () {
        setPlanRankerConfig(db); // Defaults: mixed ranking, sampling CE.
        // The capped trial produces results, so the multi-planner decides before CBR runs and the
        // capped phase is never resumed: every plan is scored on its capped-phase work alone.
        // This is the zero-work-finalize case - a phase group exists only for a phase the plan
        // did work in, so each plan carries the estimate group only, and that group holds the
        // score and the plan's final stop condition.
        const explain = explainFind(matchingFilter);
        assertChosenRanker(explain, ChosenRanker.kMultiPlanning, PlanRankerReason.kMpEarlyExit);
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        const scores = [];
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(hasNodeGroup(plan, "multiPlanEstimate"), "expected multiPlanEstimate group", {
                plan,
            });
            assert(!hasNodeGroup(plan, "multiPlanFinalize"), "unexpected multiPlanFinalize group", {
                plan,
            });
            assert(plan.multiPlanEstimateStats, "expected plan-level multiPlanEstimateStats", {
                plan,
            });
            assert(
                !plan.hasOwnProperty("multiPlanFinalizeStats"),
                "unexpected plan-level multiPlanFinalizeStats",
                {plan},
            );
            scores.push(plan.multiPlanEstimateStats.score);
        }
        assertDescending(scores.slice(1), plans);
    });

    it("mixed (default), CBR wins: merged statistics, cost-ascending order", function () {
        setPlanRankerConfig(db); // Defaults: mixed ranking, sampling CE.
        // No plan returns results during the capped trial, so the cost-based ranker decides.
        // The deciding ranker affects ordering only, never visibility: everything computed is
        // shown.
        const explain = explainFind(cbrWinFilter);
        assertChosenRanker(
            explain,
            ChosenRanker.kCostBased,
            PlanRankerReason.kNoMultiplanningResults,
        );
        const plans = getV3Plans(explain);
        // Each logical plan appears exactly once: the multi-planner's capped-trial tree and the
        // cost-based ranker's costed record of the same solution are merged into a single entry
        // whose statistics document carries both families - costBased (remapped estimates) and
        // the multi-planner phase groups. Every plan ran the capped phase, so every plan carries
        // the estimate group; only the winner ran the finishing-up trial afterwards, so it alone
        // carries the finalize group as well (with its score and final stop condition), while the
        // abandoned candidates stay estimate-only with the capped phase's budget exhaustion as
        // their final condition.
        assert.gte(plans.length, 3, "expected one entry per candidate plan", {plans});
        const costs = [];
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(hasCostBasedGroup(plan), "expected costBased on every plan", {plan});
            assert(hasNodeGroup(plan, "multiPlanEstimate"), "expected merged trial statistics", {
                plan,
            });
            assert(plan.multiPlanEstimateStats, "expected plan-level multiPlanEstimateStats", {
                plan,
            });
            costs.push(rootCostEstimate(plan));
        }
        // The winner: estimate + finalize, the capped phase having exhausted its budget (that is
        // what engaged CBR) and the finishing-up trial reporting the final condition.
        const winner = plans[0];
        assert(winner.multiPlanFinalizeStats, "expected the winner's finishing-up trial group", {
            winner,
        });
        assert(
            winner.multiPlanFinalizeStats.hasOwnProperty("score"),
            "expected the score in the winner's last group",
            {winner},
        );
        assert(
            !winner.multiPlanEstimateStats.hasOwnProperty("score"),
            "score must appear only in the last present group",
            {winner},
        );
        assertStopCondition(
            winner,
            MultiPlannerStopCondition.kExhaustedBudget,
            "multiPlanEstimateStats",
        );
        // The finalize subobject reports the finishing-up trial that collects the winner's works
        // for the plan cache. The filter matches nothing, so that trial too runs out its budget.
        assertStopCondition(
            winner,
            MultiPlannerStopCondition.kExhaustedBudget,
            "multiPlanFinalizeStats",
        );
        // The abandoned candidates: estimate-only, ended by the capped phase's budget.
        for (const plan of plans.slice(1)) {
            assert(
                !plan.hasOwnProperty("multiPlanFinalizeStats"),
                "abandoned candidates never resume",
                {plan},
            );
            assertStopCondition(plan, MultiPlannerStopCondition.kExhaustedBudget, "final");
        }
        // The plans after the winner are in cost-ascending order (the cost-based ranker decided).
        assertAscending(costs.slice(1), plans);
    });

    it("plannerChoice: same plans[] shape, structure only", function () {
        // plannerChoice renders the same plans[] shape as the stats-rich modes but withholds every
        // statistic: no per-node "statistics" grouping of either family and neither plan-level
        // multi-planner group. Asserted under both mixed-mode outcomes, since what is excluded
        // must not depend on which ranker did the deciding - only the plan *order* does.
        for (const [name, filter] of [
            ["multi-planner decided", matchingFilter],
            ["cost-based ranker decided", cbrWinFilter],
        ]) {
            setPlanRankerConfig(db); // Defaults: mixed ranking, sampling CE.
            const explain = explainFind(filter, "plannerChoice");
            assert.eq(explain.explainVersion, "3", "expected V3 version reporting", {
                explain,
                name,
            });
            const plans = getV3Plans(explain);
            assert.gte(plans.length, 2, "expected multiple candidate plans", {plans, name});
            for (const plan of plans) {
                assertWellFormedPlan(plan);
                assert(!hasMultiPlanGroup(plan), "unexpected multiPlan group", {plan, name});
                assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan, name});
                for (const field of ["multiPlanEstimateStats", "multiPlanFinalizeStats"]) {
                    assert(!plan.hasOwnProperty(field), "unexpected " + field, {plan, name});
                }
                forEachNode(plan.planStages, (node) => {
                    assert(!node.hasOwnProperty("statistics"), "unexpected statistics", {
                        node,
                        name,
                    });
                });
            }

            // The structure itself is unabridged: the same stage trees plannerStats shows.
            const statsPlans = getV3Plans(explainFind(filter, "plannerStats"));
            assert.eq(plans.length, statsPlans.length, {plans, statsPlans, name});
            const stagesOf = (plan) => {
                const stages = [];
                forEachNode(plan.planStages, (node) => stages.push(node.stage));
                return stages;
            };
            for (let i = 0; i < plans.length; ++i) {
                assert.eq(stagesOf(plans[i]), stagesOf(statsPlans[i]), {plans, statsPlans, name});
            }
        }
    });

    it("mixed (default), multi-planner resumes: disjoint estimate and finalize groups", function () {
        setPlanRankerConfig(db); // Defaults: mixed ranking, sampling CE.
        // No plan produces results within the capped phase (so CBR is engaged), but $returnKey
        // makes every plan inestimable (RETURN_KEY), so the multi-planner resumes the capped
        // trial with the remaining budget and decides. Every surviving plan therefore did work in
        // both phases and carries both groups, whose counters are disjoint: the estimate group
        // holds the capped phase's counters, the finalize group only the increments accumulated
        // after the capped phase.
        const explain = assert.commandWorked(
            db.runCommand({
                explain: {find: collName, filter: cbrWinFilter, returnKey: true},
                verbosity: "plannerStats",
            }),
        );
        assertChosenRanker(
            explain,
            ChosenRanker.kMultiPlanning,
            PlanRankerReason.kCBRInestimableNode,
        );
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(plan.multiPlanEstimateStats, "expected plan-level multiPlanEstimateStats", {
                plan,
            });
            assert(plan.multiPlanFinalizeStats, "expected plan-level multiPlanFinalizeStats", {
                plan,
            });
            // The capped phase ended for every plan by exhausting its budget - that is what
            // engaged CBR - regardless of how the resumed phase ended (the final condition,
            // reported in the finalize group).
            assertStopCondition(
                plan,
                MultiPlannerStopCondition.kExhaustedBudget,
                "multiPlanEstimateStats",
            );
            // The finalize subobject reports the resumed trial. The filter matches nothing, so
            // it too runs out its (remaining) budget.
            assertStopCondition(
                plan,
                MultiPlannerStopCondition.kExhaustedBudget,
                "multiPlanFinalizeStats",
            );
            // Disjointness at plan level: both phases examined keys, so the totals are split,
            // not repeated.
            assert.gt(plan.multiPlanEstimateStats.totalKeysExamined, 0, {plan});
            assert.gt(plan.multiPlanFinalizeStats.totalKeysExamined, 0, {plan});
            // Disjointness at node level: the root carries both groups, and the finalize works
            // are an increment (positive: the resume ran), not a cumulative repeat (it is smaller
            // than or comparable to the estimate works would make it if repeated; the exact
            // values are budget-dependent, so assert only positivity here - the exact
            // estimate + finalize == cumulative identity is unit-tested in plan_explainer_test).
            const rootStatistics = plan.planStages.statistics;
            assert(rootStatistics && rootStatistics.multiPlanEstimate, {plan});
            assert(rootStatistics && rootStatistics.multiPlanFinalize, {plan});
            assert.gt(rootStatistics.multiPlanEstimate.works, 0, {plan});
            assert.gt(rootStatistics.multiPlanFinalize.works, 0, {plan});
            // The capped phase produced no results (that is what engaged CBR); returned rows,
            // shown on the candidate's root node, are all finalize-phase increments.
            assert.eq(rootStatistics.multiPlanEstimate.nReturned, 0, "capped phase found results", {
                plan,
            });
            // The score appears only in the last present group.
            assert(
                !plan.multiPlanEstimateStats.hasOwnProperty("score"),
                "score must appear only in the last present group",
                {plan},
            );
        }
    });

    it("a stage that adds children after the capped phase serializes them finalize-only", function () {
        // Regression test for the phase-split alignment: a GEO_NEAR stage appends one child per
        // search interval it opens, so its stats tree can gain children between the capped-phase
        // snapshot and the final read - the snapshot's children are only a prefix of the final
        // ones. The explain must not reject that (it tripped tassert 13312306 before the fix),
        // and a child born after the capped phase must carry only the finalize group.
        //
        // The EstimateRankingEffort strategy provides the phase split: its estimation trial is
        // capped at internalQueryNumWorksPerPlanForMPEstimation works (squeezed to the knob's
        // minimum), and $near's GEO_NEAR_2DSPHERE stage is inestimable, so the multi-planner
        // resumes with the remaining budget. The data pins the intervals: a cluster of docs a few
        // hundred meters from the query point makes the first interval small (its radius derives
        // from the distance to the nearest docs) and keeps the capped trial inside it - scanning
        // the cluster needs several times more works than the cap. Only 30 cluster docs pass the
        // f1 filter, fewer than the trial's result target, so the resumed phase cannot fill its
        // batch from the first interval and must keep doubling the search radius - opening
        // interval after interval - until it reaches the far docs 1000+ km away, which all pass.
        const geoColl = db[collName + "_geo"];
        geoColl.drop();
        const docs = [];
        for (let i = 0; i < 400; ++i) {
            // ~300m east of the query point, jittered so the docs don't coincide.
            docs.push({_id: i, loc: {type: "Point", coordinates: [0.003 + i * 1e-7, 0]}, f1: i});
        }
        for (let i = 400; i < 800; ++i) {
            // Spread 1000-4000km away.
            docs.push({_id: i, loc: {type: "Point", coordinates: [10 + (i % 30), 0]}, f1: i});
        }
        assert.commandWorked(geoColl.insert(docs));
        assert.commandWorked(geoColl.createIndex({loc: "2dsphere"}));
        assert.commandWorked(geoColl.createIndex({f1: 1}));

        setPlanRankerConfig(db, {internalQueryMixedPlanRankingStrategy: "EstimateRankingEffort"});
        const estimationWorksParam = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryNumWorksPerPlanForMPEstimation: 1}),
        ).internalQueryNumWorksPerPlanForMPEstimation;
        try {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryNumWorksPerPlanForMPEstimation: 101,
                }),
            );
            // Passes the last 30 cluster docs and every far doc.
            const explain = assert.commandWorked(
                db.runCommand({
                    explain: {
                        find: geoColl.getName(),
                        filter: {
                            loc: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}},
                            f1: {$gte: 370},
                        },
                    },
                    verbosity: "plannerStats",
                }),
            );
            assertChosenRanker(
                explain,
                ChosenRanker.kMultiPlanning,
                PlanRankerReason.kInestimableMP,
            );
            // Find the GEO_NEAR node of a plan that ran both phases.
            let geoNearNode = null;
            for (const plan of getV3Plans(explain)) {
                assertWellFormedPlan(plan);
                forEachNode(plan.planStages, (node) => {
                    if (
                        node.stage === "GEO_NEAR_2DSPHERE" &&
                        node.statistics &&
                        node.statistics.multiPlanEstimate
                    ) {
                        geoNearNode = node;
                    }
                });
            }
            assert(geoNearNode, "expected a two-phase GEO_NEAR_2DSPHERE node", {explain});
            // The resumed phase opened intervals the snapshot never saw: those children carry
            // the finalize group only. At least one child predates the capped phase's end and carries the
            // estimate group (the capped trial ran inside the first interval).
            const children = geoNearNode.inputStages || [];
            const bornBefore = children.filter(
                (c) => c.statistics && c.statistics.multiPlanEstimate,
            );
            const bornAfter = children.filter(
                (c) =>
                    c.statistics &&
                    !c.statistics.multiPlanEstimate &&
                    c.statistics.multiPlanFinalize,
            );
            assert.gte(
                bornBefore.length,
                1,
                "expected an interval opened during the capped phase",
                {
                    geoNearNode,
                },
            );
            assert.gte(bornAfter.length, 1, "expected an interval opened after the capped phase", {
                geoNearNode,
            });
            // The two phases of the winner's trial ended differently, and each phase group
            // reports its own condition: the capped estimation trial ran out of its works budget
            // (which is why the multi-planner resumed at all), while the resumed phase ended by
            // filling the result batch from the far documents.
            const winner = getV3Plans(explain)[0];
            assertStopCondition(
                winner,
                MultiPlannerStopCondition.kExhaustedBudget,
                "multiPlanEstimateStats",
            );
            assertStopCondition(
                winner,
                MultiPlannerStopCondition.kFullBatch,
                "multiPlanFinalizeStats",
            );
        } finally {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryNumWorksPerPlanForMPEstimation: estimationWorksParam,
                }),
            );
            geoColl.drop();
        }
    });

    it("featureFlagCostBasedRanker off behaves as pure multi-planning", function () {
        setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
        const explain = explainFind(matchingFilter);
        assertChosenRanker(
            explain,
            ChosenRanker.kMultiPlanning,
            PlanRankerReason.kCBRFeatureFlagDisabled,
        );
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            // Pure multi-planning behavior: one uncapped (deciding) trial, so finalize-only.
            assert(hasNodeGroup(plan, "multiPlanFinalize"), "expected multiPlanFinalize group", {
                plan,
            });
            assert(!hasNodeGroup(plan, "multiPlanEstimate"), "unexpected multiPlanEstimate group", {
                plan,
            });
            assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan});
        }
    });

    it("stopCondition reports how each trial period ended", function () {
        setPlanRankerConfig(db, {internalQueryPlanRanker: "multiPlanning"});

        // Every candidate returns far more rows than the trial's result target and cannot reach
        // EOF within it, so the trial ends on a full batch of results.
        const fullBatchPlans = getV3Plans(explainFind(matchingFilter));
        assert.gte(fullBatchPlans.length, 2, {fullBatchPlans});
        assertStopCondition(
            fullBatchPlans[0],
            MultiPlannerStopCondition.kFullBatch,
            "multiPlanFinalizeStats",
        );

        // 'a: 0' matches 100 documents, fewer than the trial's result target, so the winning
        // candidate exhausts its results and ends its trial at EOF. Other candidates may reach EOF
        // in the same round here - the a_1_b_1 index scan is just as short as the a_1 one - so this
        // case only pins the winner.
        const eofPlans = getV3Plans(explainFind({a: 0, b: 0}));
        assert.gte(eofPlans.length, 2, {eofPlans});
        assertStopCondition(eofPlans[0], MultiPlannerStopCondition.kEof, "multiPlanFinalizeStats");

        // No document has 'a: 0' and 'b: 5' ('a % 100 === 0' implies 'b % 10 === 0'), so the
        // a_1_b_1 index scan has empty bounds and reaches EOF on its first work, winning on the EOF
        // bonus. That ends the trial period for everyone else after that single round: the a_1 and
        // b_1 plans, which would have needed 100 and 1000 works to reach EOF themselves, met no
        // early-exit condition and were cut short with nearly the whole budget unspent. That is
        // 'trialEndedEarly', not 'exhaustedBudget'.
        const cutShortPlans = getV3Plans(explainFind({a: 0, b: 5}));
        assert.gte(cutShortPlans.length, 2, {cutShortPlans});
        assertStopCondition(
            cutShortPlans[0],
            MultiPlannerStopCondition.kEof,
            "multiPlanFinalizeStats",
        );
        for (const plan of cutShortPlans.slice(1)) {
            assertStopCondition(
                plan,
                MultiPlannerStopCondition.kTrialEndedEarly,
                "multiPlanFinalizeStats",
            );
        }

        // With the trial's work budget squeezed to a single work per plan, no candidate can meet
        // an early-exit condition (full batch or EOF), so every candidate runs out of budget.
        const worksParam = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryPlanEvaluationWorks: 1}),
        ).internalQueryPlanEvaluationWorks;
        const collFractionParam = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryPlanEvaluationCollFraction: 1}),
        ).internalQueryPlanEvaluationCollFraction;
        const totalCollFractionParam = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryPlanTotalEvaluationCollFraction: 1}),
        ).internalQueryPlanTotalEvaluationCollFraction;
        try {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 1}),
            );
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryPlanEvaluationCollFraction: 0.0}),
            );
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryPlanTotalEvaluationCollFraction: 0.0,
                }),
            );
            const exhaustedPlans = getV3Plans(explainFind(matchingFilter));
            assert.gte(exhaustedPlans.length, 2, {exhaustedPlans});
            for (const plan of exhaustedPlans) {
                assertStopCondition(
                    plan,
                    MultiPlannerStopCondition.kExhaustedBudget,
                    "multiPlanFinalizeStats",
                );
            }
        } finally {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: worksParam}),
            );
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryPlanEvaluationCollFraction: collFractionParam,
                }),
            );
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryPlanTotalEvaluationCollFraction: totalCollFractionParam,
                }),
            );
        }
    });

    it("a candidate whose trial fails recoverably reports 'failed'", function () {
        setPlanRankerConfig(db, {internalQueryPlanRanker: "multiPlanning"});
        // A candidate that exceeds an allowed resource consumption mid-trial is marked failed and
        // ranked out, but the trial continues for the others and the failed candidate is still
        // displayed among the rejected plans - carrying the partial counters it accumulated, hence
        // a stop condition to report like any other plan that ran.
        //
        // 'sort: {c: 1}' is satisfied by the c_1 index (no blocking sort), while the other
        // candidates must sort. Squeezing the sort's memory limit makes those candidates fail -
        // but only with disk use disallowed, since otherwise the sort spills.
        const sortMemParam = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}),
        ).internalQueryMaxBlockingSortMemoryUsageBytes;
        const diskUseParam = assert.commandWorked(
            db.adminCommand({getParameter: 1, allowDiskUseByDefault: 1}),
        ).allowDiskUseByDefault;
        assert.commandWorked(coll.createIndex({c: 1}));
        try {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryMaxBlockingSortMemoryUsageBytes: 1000,
                    allowDiskUseByDefault: false,
                }),
            );
            const explain = assert.commandWorked(
                db.runCommand({
                    explain: {find: collName, filter: {a: {$gte: 0}}, sort: {c: 1}},
                    verbosity: "plannerStats",
                }),
            );
            const plans = getV3Plans(explain);
            assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
            for (const plan of plans) {
                assertWellFormedPlan(plan);
            }
            // Every candidate that cannot use the c_1 index has to sort and so fails; only the
            // number of such candidates depends on the index set, not the behavior under test.
            // This is a pure multi-planning trial, so each plan's single group is the
            // finalize-named one.
            const failedPlans = plans.filter(
                (plan) =>
                    plan.multiPlanFinalizeStats &&
                    plan.multiPlanFinalizeStats.stopCondition === MultiPlannerStopCondition.kFailed,
            );
            assert.gte(failedPlans.length, 1, "expected a failed candidate", {plans});
            assert.lt(failedPlans.length, plans.length, "expected a surviving candidate", {plans});
            for (const failedPlan of failedPlans) {
                // A failed candidate is never scored, so it carries counters but no score.
                assert(
                    !failedPlan.multiPlanFinalizeStats.hasOwnProperty("score"),
                    "unexpected score on a failed candidate",
                    {failedPlan},
                );
            }
            // It never wins: failed candidates are excluded from the ranking.
            assert.neq(
                plans[0].multiPlanFinalizeStats.stopCondition,
                MultiPlannerStopCondition.kFailed,
                "a failed candidate must not be the winning plan",
                {plans},
            );
        } finally {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryMaxBlockingSortMemoryUsageBytes: sortMemParam,
                    allowDiskUseByDefault: diskUseParam,
                }),
            );
            assert.commandWorked(coll.dropIndex({c: 1}));
        }
    });

    it("single plan: one well-formed entry, no ranking statistics", function () {
        setPlanRankerConfig(db); // Defaults.
        const explain = explainFind({nonexistent: 1});
        // A single candidate solution: no ranking took place, so the chosen ranker is "singlePlan"
        assertChosenRanker(explain, ChosenRanker.kSinglePlan, PlanRankerReason.kSinglePlan);
        const plans = getV3Plans(explain);
        assert.eq(plans.length, 1, "expected a single plan", {plans});
        assertWellFormedPlan(plans[0]);
        assert(!hasMultiPlanGroup(plans[0]), "unexpected multi-planner group", {plans});
        for (const group of kPlanPhaseGroups) {
            assert(!plans[0].hasOwnProperty(group), "unexpected plan-level phase group", {
                plans,
                group,
            });
        }
        // At execStats the tree must still show no counters: no trial ran, and the real-execution
        // counters live in the retained executionStats section, not in plans[].
        const execStatsPlans = getV3Plans(explainFind({nonexistent: 1}, "execStats"));
        assert.eq(execStatsPlans.length, 1, {execStatsPlans});
        assert(
            !hasMultiPlanGroup(execStatsPlans[0]),
            "unexpected multi-planner group at execStats",
            {execStatsPlans},
        );
    });

    it("cached plan: isCached on the matching entry", function () {
        setPlanRankerConfig(db); // Defaults.
        // Run the (non-explain) query twice so the winning plan enters the plan cache.
        assert.eq(coll.find(matchingFilter).itcount(), 10000);
        assert.eq(coll.find(matchingFilter).itcount(), 10000);
        const plans = getV3Plans(explainFind(matchingFilter));
        assert(
            plans.some((plan) => plan.isCached === true),
            "expected a cached plan entry",
            {plans},
        );
    });

    it("count command uses the find plans[] shape", function () {
        setPlanRankerConfig(db); // Defaults.
        // Single-plan count: the winner's tree is the executor's (no trial ran), so the COUNT
        // root stage is visible.
        const singlePlan = getV3Plans(
            assert.commandWorked(
                db.runCommand({
                    explain: {count: collName, query: {nonexistent: 1}},
                    verbosity: "plannerStats",
                }),
            ),
        );
        assert.eq(singlePlan.length, 1, {singlePlan});
        assertWellFormedPlan(singlePlan[0]);
        assert.eq(singlePlan[0].planStages.stage, "COUNT", "expected a COUNT root stage", {
            singlePlan,
        });

        // Multi-planned count: plans[] shows the candidates' TRIAL trees, which are the
        // find-shaped trees the multi-planner ranked - the COUNT root is stacked onto the winner
        // only when the final executor is built, so it appears in the retained
        // executionStats.executionStages (at execStats), not in plans[]. This mirrors the legacy
        // allPlansExecution sections, which are built from the same trial trees.
        const multiPlan = getV3Plans(
            assert.commandWorked(
                db.runCommand({
                    explain: {count: collName, query: matchingFilter},
                    verbosity: "plannerStats",
                }),
            ),
        );
        assert.gte(multiPlan.length, 2, {multiPlan});
        for (const plan of multiPlan) {
            assertWellFormedPlan(plan);
            assert(hasMultiPlanGroup(plan), "expected trial statistics", {plan});
        }
    });

    it("express-eligible query produces a single plan entry", function () {
        setPlanRankerConfig(db); // Defaults.
        // An express query is planned by the express fast path, not by the plan enumerator: it has
        // exactly one plan, ranked by nobody. It renders in the V3 shape all the same - one entry
        // in plans[], and a rankerChoice reporting that no ranking took place.
        for (const verbosity of ["plannerStats", "execStats"]) {
            const explain = assert.commandWorked(
                db.runCommand({explain: {find: collName, filter: {_id: 1}}, verbosity}),
            );
            assert.eq(explain.explainVersion, "3", "expected V3 version reporting", {explain});
            assertChosenRanker(explain, ChosenRanker.kSinglePlan, PlanRankerReason.kSinglePlan);
            assert(!explain.queryPlanner.hasOwnProperty("winningPlan"), "unexpected winningPlan", {
                explain,
                verbosity,
            });

            const plans = getV3Plans(explain);
            assert.eq(plans.length, 1, {plans, verbosity});
            const plan = plans[0];
            assertWellFormedPlan(plan);
            assert.eq(plan.isCached, false, {plan, verbosity});
            for (const field of ["multiPlanEstimateStats", "multiPlanFinalizeStats"]) {
                assert(!plan.hasOwnProperty(field), "unexpected " + field, {plan, verbosity});
            }
            assert.eq(plan.planStages.stage, "EXPRESS_IXSCAN", {plan, verbosity});
            assert.eq(plan.planStages.indexName, "_id_", {plan, verbosity});
            // Neither statistics family applies: no trial ran and the cost-based ranker never
            // estimated the plan. This holds at execStats too - the execution counters live in the
            // retained executionStats section, never fused into a V3 node.
            assert(!hasMultiPlanGroup(plan), "unexpected multiPlan group", {plan, verbosity});
            assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan, verbosity});
            forEachNode(plan.planStages, (node) => {
                assert(!node.hasOwnProperty("statistics"), "unexpected statistics", {
                    node,
                    verbosity,
                });
            });
        }
    });

    it("express-eligible query still reports its execution counters at execStats", function () {
        setPlanRankerConfig(db); // Defaults.
        // The counters the express plan collects are not lost by the move to plans[]: they are
        // reported by the retained legacy executionStats section, which V3 execStats emits
        // unchanged.
        const explain = assert.commandWorked(
            db.runCommand({explain: {find: collName, filter: {_id: 1}}, verbosity: "execStats"}),
        );
        assert(explain.hasOwnProperty("executionStats"), "missing executionStats", {explain});
        assert.eq(explain.executionStats.nReturned, 1, {explain});
        assert.eq(explain.executionStats.executionStages.stage, "EXPRESS_IXSCAN", {explain});
    });

    it("trivial EOF plan produces a single well-formed entry", function () {
        setPlanRankerConfig(db); // Defaults.
        const emptyCollName = collName + "_eof";
        db[emptyCollName].drop();
        // A find on a nonexistent collection plans a trivial EOF stage.
        const explain = assert.commandWorked(
            db.runCommand({
                explain: {find: emptyCollName, filter: {a: 1}},
                verbosity: "plannerStats",
            }),
        );
        const plans = getV3Plans(explain);
        assert.eq(plans.length, 1, {plans});
        assertWellFormedPlan(plans[0]);
        assert.eq(plans[0].planStages.stage, "EOF", "expected a trivial EOF plan", {plans});
    });

    it("subplanned top-level $or is rejected cleanly", function () {
        setPlanRankerConfig(db); // Defaults.
        // Rooted $or queries are subplanned and not yet supported at the V3 verbosities: the
        // server rejects them with a clean error rather than producing partial output.
        // TODO SERVER-131818: once supported, assert well-formed plans[] output instead.
        for (const verbosity of ["plannerStats", "execStats"]) {
            assert.commandFailedWithCode(
                db.runCommand({
                    explain: {find: collName, filter: {$or: [{a: 1}, {b: 1}]}},
                    verbosity,
                }),
                [13145000, 13145001],
            );
        }
    });
});
