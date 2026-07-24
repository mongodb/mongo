/**
 * Tests the V3 "queryPlanner.plans" array. It runs the same query under every plan-ranker mode
 * (pure multi-planning, strict CBR with sampling and heuristic CE, and the default mixed mode
 * with both of its outcomes), plus the special plan cases (cached plan, single plan,
 * subplanned $or, EOF, count, express). For each resulting explain it asserts:
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
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";

const collName = jsTestName();
const coll = db[collName];

// Local V3 accessors. The shared analyze_plan.js helpers are winningPlan-shaped by design and
// are not converted to the V3 format (test-infra conversion is out of scope).

// Returns the V3 per-plan array of 'explain'.
function getPlans(explain) {
    assert(explain.queryPlanner, "missing queryPlanner", {explain});
    const plans = explain.queryPlanner.plans;
    assert(Array.isArray(plans), "missing queryPlanner.plans", {explain});
    assert.gte(plans.length, 1, "plans must hold at least the winning plan", {explain});
    return plans;
}

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
        const plans = getPlans(explainFind(matchingFilter));
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
        const plans = getPlans(explainFind(matchingFilter));
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
        const plans = getPlans(explainFind(matchingFilter));
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
        const plans = getPlans(explainFind(matchingFilter));
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
        const plans = getPlans(explain);
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

    it("featureFlagCostBasedRanker off behaves as pure multi-planning", function () {
        setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
        const plans = getPlans(explainFind(matchingFilter));
        assert.gte(plans.length, 2, "expected multiple candidate plans", {plans});
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(hasMultiPlanGroup(plan), "expected multiPlan node group", {plan});
            assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan});
        }
    });

    it("single plan: one well-formed entry, no ranking statistics", function () {
        setPlanRankerConfig(db); // Defaults.
        const plans = getPlans(explainFind({nonexistent: 1}));
        assert.eq(plans.length, 1, "expected a single plan", {plans});
        assertWellFormedPlan(plans[0]);
        assert(!hasMultiPlanGroup(plans[0]), "unexpected multiPlan group", {plans});
        assert(!plans[0].hasOwnProperty("multiPlanStats"), "unexpected multiPlanStats", {plans});
        // At execStats the tree must still show no counters: no trial ran, and the real-execution
        // counters live in the retained executionStats section, not in plans[].
        const execStatsPlans = getPlans(explainFind({nonexistent: 1}, "execStats"));
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
        const plans = getPlans(explainFind(matchingFilter));
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
        const singlePlan = getPlans(
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
        const multiPlan = getPlans(
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
        const plans = getPlans(explain);
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
