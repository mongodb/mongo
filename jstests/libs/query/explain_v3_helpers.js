/**
 * Helper functions related to the V3 explain output.
 */

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

export function hasMultiPlanGroup(plan) {
    let found = false;
    forEachNode(plan.planStages, (node) => {
        if (node.statistics && node.statistics.multiPlan !== undefined) {
            found = true;
        }
    });
    return found;
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
