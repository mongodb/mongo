/**
 * Tests the V3 "queryPlanner.plans" array. It runs the same query under every plan-ranker mode
 * (pure multi-planning, strict CBR with sampling and heuristic CE, and the default mixed mode
 * with both of its outcomes), plus the special plan cases (cached plan, single plan,
 * subplanned $or, EOF, count, express). Unless a case says otherwise the verbosity is
 * plannerStats; plannerChoice, which renders the same shape with every statistic withheld, has its
 * own case. For each resulting explain it asserts:
 *
 * - The per-plan object layout: {isCached, solutionHashUnstable, multiPlanStats, planStages}.
 * - The per-node "statistics" grouping: {costBased, multiPlan}. The grouping is sparse - a
 *   group is present iff the corresponding statistic was computed for that node, and the
 *   "statistics" wrapper is absent when both groups are.
 * - The ordering of the plans after the winner: by the deciding ranker's metric (trial score
 *   descending when the multi-planner decided, cost ascending when the cost-based ranker did).
 *
 * The classic engine is pinned. SBE-eligible queries currently keep legacy-shaped sections under
 * explainVersion "3".
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertChosenRanker,
    assertStopCondition,
    getV3Plans,
    MultiPlannerStopCondition,
    ChosenRanker,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";

const collName = jsTestName();
const coll = db[collName];

// Local V3 accessors. The shared analyze_plan.js helpers are winningPlan-shaped by design and
// are not converted to the V3 format (test-infra conversion is out of scope).

// Invokes 'callback' on every node of a V3 plan stage tree, root to leaves. The V3 node shape
// always nests children as the 'inputStages' array.
function forEachNode(node, callback) {
    callback(node);
    for (const child of node.inputStages || []) {
        forEachNode(child, callback);
    }
}

// Basic well-formedness of one plans[] entry per the frozen V3 shape.
function assertWellFormedPlan(plan) {
    assert(plan.hasOwnProperty("isCached"), "missing isCached", {plan});
    assert(plan.hasOwnProperty("planStages"), "missing planStages", {plan});
    // Plans reaching here range over every ranker mode, so whether this one ran a trial is not
    // known. A plan that did - equivalently, one carrying 'multiPlanStats' - must say how its trial
    // ended; a plan that never ran one has no stop condition to report.
    if (plan.hasOwnProperty("multiPlanStats")) {
        assert(
            plan.multiPlanStats.hasOwnProperty("stopCondition"),
            "expected multiPlanStats.stopCondition on a plan that ran a trial",
            {plan},
        );
    }
    forEachNode(plan.planStages, (node) => {
        assert(node.hasOwnProperty("stage"), "node missing stage", {node});
        // Counters never appear flat on the node in the V3 shape.
        assert(!node.hasOwnProperty("works"), "counter leaked out of statistics", {node});
        assert(!node.hasOwnProperty("nReturned"), "counter leaked out of statistics", {node});
        if (node.hasOwnProperty("statistics")) {
            assert(
                node.statistics.hasOwnProperty("costBased") ||
                    node.statistics.hasOwnProperty("multiPlan"),
                "statistics present but empty",
                {node},
            );
        }
    });
}

function hasMultiPlanGroup(plan) {
    let found = false;
    forEachNode(plan.planStages, (node) => {
        if (node.statistics && node.statistics.multiPlan !== undefined) {
            found = true;
        }
    });
    return found;
}

function hasCostBasedGroup(plan) {
    let found = false;
    forEachNode(plan.planStages, (node) => {
        if (node.statistics && node.statistics.costBased !== undefined) {
            found = true;
        }
    });
    return found;
}

function rootCostEstimate(plan) {
    const statistics = plan.planStages.statistics;
    assert(statistics && statistics.costBased, "missing root costBased group", {plan});
    return statistics.costBased.costEstimate;
}

// Asserts the values are non-increasing (descending order allowing ties).
function assertDescending(values, context) {
    for (let i = 1; i < values.length; ++i) {
        assert.lte(values[i], values[i - 1], "expected descending order", {values, context});
    }
}

// Asserts the values are non-decreasing (ascending order allowing ties).
function assertAscending(values, context) {
    for (let i = 1; i < values.length; ++i) {
        assert.gte(values[i], values[i - 1], "expected ascending order", {values, context});
    }
}

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

describe("V3 queryPlanner.plans array", function () {
    let savedRankerConfig;
    let savedFrameworkControl;

    before(function () {
        savedRankerConfig = getPlanRankerConfig(db);
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        // Pin the classic engine: SBE-eligible queries keep legacy-shaped output until
        // SERVER-132033.
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

    it("pure multi-planning: trial groups and score-descending order", function () {
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
            // Every candidate ran a trial: node-level multiPlan groups and plan-level
            // multiPlanStats with a score; the cost-based ranker never ran, so no costBased.
            assert(hasMultiPlanGroup(plan), "expected multiPlan node group", {plan});
            assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan});
            assert(plan.multiPlanStats, "expected plan-level multiPlanStats", {plan});
            assert(plan.multiPlanStats.hasOwnProperty("score"), "expected trial score", {plan});
            scores.push(plan.multiPlanStats.score);
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
            assert(!hasMultiPlanGroup(plan), "unexpected multiPlan group", {plan});
            assert(!plan.hasOwnProperty("multiPlanStats"), "unexpected multiPlanStats", {plan});
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

    it("mixed (default), multi-planner wins: trial groups, score-descending order", function () {
        setPlanRankerConfig(db); // Defaults: mixed ranking, sampling CE.
        // The trial produces results, so the multi-planner decides before CBR runs.
        const explain = explainFind(matchingFilter);
        assertChosenRanker(explain, ChosenRanker.kMultiPlanning, PlanRankerReason.kMpEarlyExit);
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        const scores = [];
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(hasMultiPlanGroup(plan), "expected multiPlan node group", {plan});
            assert(plan.multiPlanStats, "expected plan-level multiPlanStats", {plan});
            scores.push(plan.multiPlanStats.score);
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
        // multiPlan (the capped/abandoned trial counters; the winner shows its finishing trial).
        assert.gte(plans.length, 3, "expected one entry per candidate plan", {plans});
        const costs = [];
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(hasCostBasedGroup(plan), "expected costBased on every plan", {plan});
            assert(hasMultiPlanGroup(plan), "expected merged trial statistics", {plan});
            assert(plan.multiPlanStats, "expected plan-level multiPlanStats", {plan});
            costs.push(rootCostEstimate(plan));
        }
        // The plans after the winner are in cost-ascending order (the cost-based ranker decided).
        assertAscending(costs.slice(1), plans);
    });

    it("plannerChoice: same plans[] shape, structure only", function () {
        // plannerChoice renders the same plans[] shape as the stats-rich modes but withholds every
        // statistic: no per-node "statistics" grouping of either family and no plan-level
        // "multiPlanStats". Asserted under both mixed-mode outcomes, since what is excluded must not
        // depend on which ranker did the deciding - only the plan *order* does.
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
                assert(!plan.hasOwnProperty("multiPlanStats"), "unexpected multiPlanStats", {
                    plan,
                    name,
                });
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
            assert(hasMultiPlanGroup(plan), "expected multiPlan node group", {plan});
            assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan});
        }
    });

    it("stopCondition reports how each trial period ended", function () {
        setPlanRankerConfig(db, {internalQueryPlanRanker: "multiPlanning"});

        // Every candidate returns far more rows than the trial's result target and cannot reach
        // EOF within it, so the trial ends on a full batch of results.
        const fullBatchPlans = getV3Plans(explainFind(matchingFilter));
        assert.gte(fullBatchPlans.length, 2, {fullBatchPlans});
        assertStopCondition(fullBatchPlans[0], MultiPlannerStopCondition.kFullBatch);

        // 'a: 0' matches 100 documents, fewer than the trial's result target, so the winning
        // candidate exhausts its results and ends its trial at EOF. Other candidates may reach EOF
        // in the same round here - the a_1_b_1 index scan is just as short as the a_1 one - so this
        // case only pins the winner.
        const eofPlans = getV3Plans(explainFind({a: 0, b: 0}));
        assert.gte(eofPlans.length, 2, {eofPlans});
        assertStopCondition(eofPlans[0], MultiPlannerStopCondition.kEof);

        // No document has 'a: 0' and 'b: 5' ('a % 100 === 0' implies 'b % 10 === 0'), so the
        // a_1_b_1 index scan has empty bounds and reaches EOF on its first work, winning on the EOF
        // bonus. That ends the trial period for everyone else after that single round: the a_1 and
        // b_1 plans, which would have needed 100 and 1000 works to reach EOF themselves, met no
        // early-exit condition and were cut short with nearly the whole budget unspent. That is
        // 'trialEndedEarly', not 'exhaustedBudget'.
        const cutShortPlans = getV3Plans(explainFind({a: 0, b: 5}));
        assert.gte(cutShortPlans.length, 2, {cutShortPlans});
        assertStopCondition(cutShortPlans[0], MultiPlannerStopCondition.kEof);
        for (const plan of cutShortPlans.slice(1)) {
            assertStopCondition(plan, MultiPlannerStopCondition.kTrialEndedEarly);
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
                assertStopCondition(plan, MultiPlannerStopCondition.kExhaustedBudget);
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
            const failedPlans = plans.filter(
                (plan) =>
                    plan.multiPlanStats &&
                    plan.multiPlanStats.stopCondition === MultiPlannerStopCondition.kFailed,
            );
            assert.gte(failedPlans.length, 1, "expected a failed candidate", {plans});
            assert.lt(failedPlans.length, plans.length, "expected a surviving candidate", {plans});
            for (const failedPlan of failedPlans) {
                // A failed candidate is never scored, so it carries counters but no score.
                assert(
                    !failedPlan.multiPlanStats.hasOwnProperty("score"),
                    "unexpected score on a failed candidate",
                    {failedPlan},
                );
            }
            // It never wins: failed candidates are excluded from the ranking.
            assert.neq(
                plans[0].multiPlanStats.stopCondition,
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
        assert(!hasMultiPlanGroup(plans[0]), "unexpected multiPlan group", {plans});
        assert(!plans[0].hasOwnProperty("multiPlanStats"), "unexpected multiPlanStats", {plans});
        // At execStats the tree must still show no counters: no trial ran, and the real-execution
        // counters live in the retained executionStats section, not in plans[].
        const execStatsPlans = getV3Plans(explainFind({nonexistent: 1}, "execStats"));
        assert.eq(execStatsPlans.length, 1, {execStatsPlans});
        assert(!hasMultiPlanGroup(execStatsPlans[0]), "unexpected multiPlan group at execStats", {
            execStatsPlans,
        });
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

    it("count command rides the find shape", function () {
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

    it("express-eligible query keeps the legacy fallback shape under explainVersion 3", function () {
        setPlanRankerConfig(db); // Defaults.
        // The Express explainer does not implement the per-plan enumerator yet (SERVER-132033),
        // so it falls back to the legacy-shaped queryPlanner under explainVersion "3".
        const explain = assert.commandWorked(
            db.runCommand({explain: {find: collName, filter: {_id: 1}}, verbosity: "plannerStats"}),
        );
        assert.eq(explain.explainVersion, "3", "expected V3 version reporting", {explain});
        assert(
            explain.queryPlanner.hasOwnProperty("winningPlan"),
            "expected the legacy fallback shape",
            {explain},
        );
        assert(!explain.queryPlanner.hasOwnProperty("plans"), "unexpected V3 plans array", {
            explain,
        });
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
