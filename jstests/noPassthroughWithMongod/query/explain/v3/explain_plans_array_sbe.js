/**
 * Tests the V3 explain output for queries which use SBE.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertChosenRanker,
    ChosenRanker,
    getV3Plans,
    normalizeRunVarying,
} from "jstests/libs/query/analyze_plan.js";
import {
    assertDescending,
    assertWellFormedPlan,
    hasCostBasedGroup,
    hasMultiPlanGroup,
    kPlanPhaseGroups,
    stagesByNodeId,
    stageNames,
} from "jstests/libs/query/explain_v3_helpers.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";

// With featureFlagSbeFull the SBE runtime planner ranks the candidates instead of the classic
// multi-planner's capped trial, which changes the trial-phase output asserted below.
const sbeFull = checkSbeFullFeatureFlagEnabled(db);

const collName = jsTestName();
const coll = db[collName];

function explainFind(filter, verbosity = "plannerStats") {
    return assert.commandWorked(
        db.runCommand({explain: {find: collName, filter: filter}, verbosity}),
    );
}

describe("V3 queryPlanner.plans array on the SBE engine", function () {
    let savedFrameworkControl;
    let savedYieldIterations;
    let savedYieldPeriodMS;

    before(function () {
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
        );

        // The execStats case below compares two separate invocations of the same query for deep
        // equality, so both must plan identically. Yields can throw off the counters so
        // effectively disable yielding with large enough iterations/periodMS.
        const savedYieldParams = assert.commandWorked(
            db.adminCommand({
                getParameter: 1,
                internalQueryExecYieldIterations: 1,
                internalQueryExecYieldPeriodMS: 1,
            }),
        );
        savedYieldIterations = savedYieldParams.internalQueryExecYieldIterations;
        savedYieldPeriodMS = savedYieldParams.internalQueryExecYieldPeriodMS;
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: 1000000000,
                internalQueryExecYieldPeriodMS: 1000000000,
            }),
        );

        coll.drop();
        const docs = [];
        for (let i = 0; i < 1000; ++i) {
            docs.push({_id: i, a: i % 100, b: i % 10});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    });

    after(function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: savedFrameworkControl,
                internalQueryExecYieldIterations: savedYieldIterations,
                internalQueryExecYieldPeriodMS: savedYieldPeriodMS,
            }),
        );
        coll.drop();
    });

    // An SBE-lowered aggregation: $group is not supported by the classic engine's find path, so this
    // pipeline is guaranteed to run in SBE rather than silently falling back to classic.
    function explainSbeAgg(match, verbosity = "plannerStats") {
        return assert.commandWorked(
            db.runCommand({
                explain: {
                    aggregate: collName,
                    pipeline: [{$match: match}, {$group: {_id: "$a", c: {$sum: 1}}}],
                    cursor: {},
                },
                verbosity,
            }),
        );
    }

    it("multi-planned SBE query: trial groups, winner carries slotBasedPlan", function () {
        const explain = explainSbeAgg({a: {$gte: 0}, b: {$gte: 0}});
        assert.eq(explain.explainVersion, "3", {explain});
        const plans = getV3Plans(explain);
        assert.gte(plans.length, 2, {plans});

        // With featureFlagSbeFull the SBE runtime planner runs the trial in one uncapped phase,
        // so each plan carries only 'multiPlanFinalizeStats'. Otherwise the default mixed
        // ranker's capped trial produces results and decides without a resumed phase, so each
        // plan carries only 'multiPlanEstimateStats'.
        const [phaseGroup, absentGroup] = sbeFull
            ? ["multiPlanFinalizeStats", "multiPlanEstimateStats"]
            : ["multiPlanEstimateStats", "multiPlanFinalizeStats"];
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            assert(plan.hasOwnProperty(phaseGroup), "expected " + phaseGroup, {plan});
            assert(!plan.hasOwnProperty(absentGroup), "unexpected " + absentGroup, {plan});
            assert(hasMultiPlanGroup(plan), "expected a per-node multiPlan group", {plan});
        }

        assert(plans[0].hasOwnProperty("slotBasedPlan"), "winner must carry slotBasedPlan", {
            plans,
        });
        assert(plans[0].slotBasedPlan.hasOwnProperty("stages"), "malformed slotBasedPlan", {plans});
        for (let i = 1; i < plans.length; ++i) {
            assert(
                !plans[i].hasOwnProperty("slotBasedPlan"),
                "only the winner may carry slotBasedPlan",
                {plans},
            );
        }

        // The multi-planner decided, so the plans after the winner are ordered by trial score,
        // descending.
        assertChosenRanker(explain, ChosenRanker.kMultiPlanning);
        assertDescending(
            plans.slice(1).map((plan) => plan[phaseGroup].score),
            "SBE score order",
        );
    });

    it("single-plan SBE query: one entry from its QuerySolution, no trial statistics", function () {
        const explain = explainSbeAgg({nonexistent: 1});
        const plans = getV3Plans(explain);
        assert.eq(plans.length, 1, {plans});
        assertWellFormedPlan(plans[0]);
        for (const group of kPlanPhaseGroups) {
            assert(!plans[0].hasOwnProperty(group), "unexpected plan-level phase group", {plans});
        }
        assert(!hasMultiPlanGroup(plans[0]), "unexpected multiPlan group", {plans});
        assert(plans[0].hasOwnProperty("slotBasedPlan"), "winner must carry slotBasedPlan", {
            plans,
        });
    });

    // plans[] describes the plans as ranked. With a pushed-down pipeline that is not the tree that
    // runs: the pipeline extends the plan upward after ranking, and lowering then rewrites it. The
    // two are reported separately, and 'executedPlanStages' is present only when they differ.
    it("plans[] holds the ranked tree; the winner carries the executed tree", function () {
        for (const {label, match} of [
            {label: "multi-planned", match: {a: {$gte: 0}, b: {$gte: 0}}},
            {label: "single-plan", match: {nonexistent: 1}},
        ]) {
            const plans = getV3Plans(explainSbeAgg(match));
            const context = {label, plans};

            // The pushed-down $group was never ranked, so it is not in the ranked tree.
            const ranked = stageNames(plans[0].planStages);
            assert(!ranked.includes("GROUP"), "$group must not appear in the ranked tree", context);

            assert(
                plans[0].hasOwnProperty("executedPlanStages"),
                "winner must carry the executed tree when a pipeline was pushed down",
                context,
            );
            const executed = stageNames(plans[0].executedPlanStages);
            assert(executed.includes("GROUP"), "$group must appear in the executed tree", context);

            // Only the winner: the plans after it were ranked and rejected, so none of them ran.
            for (let i = 1; i < plans.length; ++i) {
                assert(
                    !plans[i].hasOwnProperty("executedPlanStages"),
                    "only the winner may carry the executed tree",
                    context,
                );
            }

            // With featureFlagSbeFull the ranked entries come from QuerySolutions with no node
            // ids assigned, so the trees cannot be correlated by id.
            if (sbeFull) {
                continue;
            }

            // Node ids are assigned post-order, so extending the plan and rewriting it leave the
            // find part's ids untouched: the nodes the two trees share are correlatable by id.
            const rankedIds = stagesByNodeId(plans[0].planStages);
            const executedIds = stagesByNodeId(plans[0].executedPlanStages);
            const shared = Object.keys(rankedIds).filter((s) => s in executedIds);
            assert.gt(shared.length, 0, "expected the two trees to share a node", context);
            for (const stage of shared) {
                assert.eq(
                    rankedIds[stage],
                    executedIds[stage],
                    `planNodeId disagrees between the ranked and executed tree for ${stage}`,
                    context,
                );
            }
        }
    });

    it("no executed tree when the plan that ran is the plan that was ranked", function () {
        // Nothing is pushed down for a plain find, so there is only one tree to report.
        const plans = getV3Plans(explainFind({a: {$gte: 0}, b: {$gte: 0}}));
        for (const plan of plans) {
            assert(
                !plan.hasOwnProperty("executedPlanStages"),
                "executedPlanStages must be absent when it would duplicate planStages",
                {plans},
            );
        }
    });

    it("Warn if the SBE plan is too large to include", function () {
        const original = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryExplainSizeThresholdBytes: 1}),
        ).internalQueryExplainSizeThresholdBytes;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryExplainSizeThresholdBytes: 10}),
        );
        try {
            const plans = getV3Plans(explainSbeAgg({a: {$gte: 0}, b: {$gte: 0}}));
            assert(
                !plans[0].hasOwnProperty("slotBasedPlan"),
                "slotBasedPlan should not fit in the budget",
                {plans},
            );
            assert.eq(
                plans[0].warning,
                "slotBasedPlan exceeded BSON size limit for explain",
                "expected the plan-level size warning",
                {plans},
            );
        } finally {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryExplainSizeThresholdBytes: original,
                }),
            );
        }
    });

    it("plannerChoice on SBE withholds every statistic", function () {
        const plans = getV3Plans(explainSbeAgg({a: {$gte: 0}, b: {$gte: 0}}, "plannerChoice"));
        assert.gte(plans.length, 2, {plans});
        for (const plan of plans) {
            assertWellFormedPlan(plan);
            for (const group of kPlanPhaseGroups) {
                assert(!plan.hasOwnProperty(group), "unexpected plan-level phase group", {plan});
            }
            assert(!hasMultiPlanGroup(plan), "unexpected multiPlan group", {plan});
            assert(!hasCostBasedGroup(plan), "unexpected costBased group", {plan});
        }
    });

    it("execStats on SBE renders the same plans[] as plannerStats", function () {
        const filter = {a: {$gte: 0}, b: {$gte: 0}};
        const plannerStats = explainSbeAgg(filter, "plannerStats");
        const execStats = explainSbeAgg(filter, "execStats");

        // The plans[] array in the queryPlanner section must be identical regardless of whether
        // the verbosity requires runtime execution stats.
        assert.docEq(
            normalizeRunVarying(plannerStats.queryPlanner),
            normalizeRunVarying(execStats.queryPlanner),
            "queryPlanner must be identical between plannerStats and execStats",
        );

        assert(execStats.hasOwnProperty("executionStats"), "missing executionStats", {execStats});
        assert(
            !execStats.executionStats.hasOwnProperty("allPlansExecution"),
            "unexpected allPlansExecution",
            {execStats},
        );
    });
});
