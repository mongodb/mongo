/**
 * Helper functions related to the V3 explain output.
 */
import {isV3QueryPlanner} from "jstests/libs/query/analyze_plan.js";

// The V3 verbosities that carry the multi-planning trial statistics on the plans. plannerChoice
// shows the plan structure but withholds these; plannerStats/execStats add them.
export const kPlannerStatsVerbosities = ["plannerStats", "execStats"];

// Invokes 'callback' on every node of a V3 plan stage tree, root to leaves. The V3 node shape
// always nests children as the 'inputStages' array.
export function forEachNode(node, callback) {
    callback(node);
    for (const child of node.inputStages || []) {
        forEachNode(child, callback);
    }
}

// The stage names of a V3 plan stage tree, root to leaves.
export function stageNames(tree) {
    const names = [];
    forEachNode(tree, (node) => names.push(node.stage));
    return names;
}

// Maps planNodeId to stage name for a V3 plan stage tree.
export function stagesByNodeId(tree) {
    const stages = {};
    forEachNode(tree, (node) => {
        assert(!(node.planNodeId in stages), "planNodeId is not unique within the tree", {
            tree,
            node,
        });
        stages[node.planNodeId] = node.stage;
    });
    return stages;
}

// Basic well-formedness of one plans[] entry per the frozen V3 shape.
export function assertWellFormedPlan(plan) {
    assert(plan.hasOwnProperty("isCached"), "missing isCached", {plan});
    assert(plan.hasOwnProperty("planStages"), "missing planStages", {plan});
    // The executed tree, when the plan carries one, uses the same node shape as 'planStages'.
    if (plan.hasOwnProperty("executedPlanStages")) {
        forEachNode(plan.executedPlanStages, (node) => {
            assert(node.hasOwnProperty("stage"), "executed tree node missing stage", {node});
        });
    }
    // Plans reaching here range over every ranker mode, so whether this one ran a trial is not
    // known. A plan that did - equivalently, one carrying a plan-level phase group - must say how
    // that phase ended; a plan that never ran a trial has no stop condition to report.
    for (const group of kPlanPhaseGroups) {
        if (plan.hasOwnProperty(group)) {
            assert(
                plan[group].hasOwnProperty("stopCondition"),
                "expected a stopCondition on every present phase group",
                {plan, group},
            );
        }
    }
    forEachNode(plan.planStages, (node) => {
        assert(node.hasOwnProperty("stage"), "node missing stage", {node});
        // Counters never appear flat on the node in the V3 shape.
        assert(!node.hasOwnProperty("works"), "counter leaked out of statistics", {node});
        assert(!node.hasOwnProperty("nReturned"), "counter leaked out of statistics", {node});
        if (node.hasOwnProperty("statistics")) {
            assert(
                node.statistics.hasOwnProperty("costBased") ||
                    node.statistics.hasOwnProperty("multiPlanEstimate") ||
                    node.statistics.hasOwnProperty("multiPlanFinalize"),
                "statistics present but empty",
                {node},
            );
        }
    });
}

// The plan-level multi-planner phase groups, in phase order: the capped (estimate) trial phase
// and the deciding (finalize) phase. Each is present iff the plan did work in that phase.
export const kPlanPhaseGroups = ["multiPlanEstimateStats", "multiPlanFinalizeStats"];

// Asserts that 'queryPlanner' is in the V3 shape and reports statistics consistent with
// 'verbosity'.
export function assertV3QueryPlanner(queryPlanner, verbosity, context) {
    assert(isV3QueryPlanner(queryPlanner), "expected the V3 format", context);
    assert.gt(queryPlanner.plans.length, 0, "empty 'plans' array", context);
    assert(
        !queryPlanner.hasOwnProperty("rejectedPlans"),
        "legacy 'rejectedPlans' in V3 output",
        context,
    );
    assert(queryPlanner.hasOwnProperty("rankerChoice"), "missing 'rankerChoice'", context);

    for (const plan of queryPlanner.plans) {
        assert(plan.hasOwnProperty("planStages"), "plan is missing 'planStages'", context);
        if (!kPlannerStatsVerbosities.includes(verbosity)) {
            for (const group of kPlanPhaseGroups) {
                assert(
                    !plan.hasOwnProperty(group),
                    `unexpected '${group}' at plannerChoice`,
                    context,
                );
            }
        }
    }
}

// Whether any node of the plan's tree carries the named per-node statistics group
// ("multiPlanEstimate", "multiPlanFinalize", or "costBased").
export function hasNodeGroup(plan, name) {
    let found = false;
    forEachNode(plan.planStages, (node) => {
        if (node.statistics && node.statistics[name] !== undefined) {
            found = true;
        }
    });
    return found;
}

// Whether any node of the plan's tree carries a multi-planner statistics group of either phase.
export function hasMultiPlanGroup(plan) {
    return hasNodeGroup(plan, "multiPlanEstimate") || hasNodeGroup(plan, "multiPlanFinalize");
}

export function hasCostBasedGroup(plan) {
    let found = false;
    forEachNode(plan.planStages, (node) => {
        if (node.statistics && node.statistics.costBased !== undefined) {
            found = true;
        }
    });
    return found;
}

// Returns the cost estimate for plan's root node. Assumes 'plan' has cost estimates at the path
// 'planStages.statistics.costBased.costEstimate'.
export function rootCostEstimate(plan) {
    const statistics = plan.planStages.statistics;
    assert(statistics && statistics.costBased, "missing root costBased group", {plan});
    return statistics.costBased.costEstimate;
}

// Asserts the values are non-increasing (descending order allowing ties).
export function assertDescending(values, context) {
    for (let i = 1; i < values.length; ++i) {
        assert.lte(values[i], values[i - 1], "expected descending order", {values, context});
    }
}

// Asserts the values are non-decreasing (ascending order allowing ties).
export function assertAscending(values, context) {
    for (let i = 1; i < values.length; ++i) {
        assert.gte(values[i], values[i - 1], "expected ascending order", {values, context});
    }
}
