/**
 * Tests the new "version 3" (V3) explain verbosity modes: planSummary, plannerChoice, plannerStats,
 * execStats.
 *
 * Data-driven: `testQueries` lists the queries as plain command documents, and
 * `verbosityExpectations` maps each verbosity to the explain version it reports and the highest
 * explain section it should produce. The legacy sections are inclusive,
 * queryPlanner ⊂ executionStats ⊂ allPlansExecution: a verbosity that reaches a section includes
 * that section and every section before it, and none after. The test runs every query under every
 * verbosity.
 *
 * Each V3 mode reports "explainVersion: '3'". On the find path,
 * plannerStats/execStats render the real V3 queryPlanner (a "plans" array); plannerStats does not
 * execute the query and has no executionStats section, while execStats adds exactly the retained
 * legacy kExecStats section (never an allPlansExecution array - that content lives in
 * queryPlanner.plans[]). planSummary/plannerChoice remain legacy-delegated (-> queryPlanner)
 * until SERVER-131451. The aggregation path remains legacy-delegated end-to-end
 * (TODO SERVER-130810), so not-fully-lowered pipelines keep the mapped legacy shapes
 * (plannerStats -> allPlansExecution, still executing the pipeline).
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

const collName = jsTestName();

// Explain sections in inclusive order: reaching a section implies all earlier ones are present.
const SECTIONS = ["queryPlanner", "executionStats", "allPlansExecution"];

// Queries under test. Add a new one by appending a plain command document; the runner wraps it in
// {explain: <command>, verbosity: <v>}.
const testQueries = [
    {name: "find", command: {find: collName, filter: {a: 2}}},
    {name: "agg-find", command: {aggregate: collName, pipeline: [{$match: {a: 2}}], cursor: {}}},
    {
        // A pipeline forced to stay a classic DocumentSource pipeline (via
        // $_internalInhibitOptimization) so DocumentSourceCursor::serialize() runs and the explain
        // sections live under the $cursor stage.
        name: "agg-pipeline",
        command: {
            aggregate: collName,
            pipeline: [{$_internalInhibitOptimization: {}}, {$group: {_id: "$a", c: {$sum: 1}}}],
            cursor: {},
        },
    },
    {
        // A $unionWith followed by a $match: the trailing $match is duplicated across the union in
        // DocumentSourceUnionWith::optimizeAt(), whose explain bookkeeping (_pushedDownStages) is
        // driven by the policy of the originally requested (possibly V3) verbosity, while the emit side
        // (serialize()) runs at the translated legacy verbosity until the aggregation path produces
        // real V3 output. This case guards that the V3 planner-side modes (which now report
        // hasExecStats() == false and record no pushed-down stages) still emit no execution content
        // and keep producing a well-formed explain.
        // TODO SERVER-130810 once the aggregate path threads the real V3 verbosity end-to-end.
        name: "agg-unionWith",
        command: {
            aggregate: collName,
            pipeline: [
                {$unionWith: {coll: collName, pipeline: [{$match: {a: 2}}]}},
                {$match: {b: {$gte: 0}}},
            ],
            cursor: {},
        },
    },
];

// Per verbosity: the reported explain version and the highest (inclusive) section it produces.
// 'topSectionByQuery' overrides the default for queries on paths with a different fidelity (the
// legacy-delegated aggregation path, TODO SERVER-130810).
const V3 = "3";
const LEGACY = ["1", "2"]; // engine-determined (Classic / SBE)
const verbosityExpectations = {
    // V3 modes.
    planSummary: {version: V3, topSection: "queryPlanner"},
    plannerChoice: {version: V3, topSection: "queryPlanner"},
    // The find path emits no execution sections at all (trial statistics live in
    // queryPlanner.plans[] and the query is not executed); the aggregation path still executes
    // and renders the mapped legacy allPlansExecution shape for not-fully-lowered pipelines.
    plannerStats: {
        version: V3,
        topSection: "queryPlanner",
        topSectionByQuery: {
            "agg-pipeline": "allPlansExecution",
            "agg-unionWith": "allPlansExecution",
        },
    },
    execStats: {version: V3, topSection: "executionStats"},
    // Legacy modes, for regression coverage.
    queryPlanner: {version: LEGACY, topSection: "queryPlanner"},
    executionStats: {version: LEGACY, topSection: "executionStats"},
    allPlansExecution: {version: LEGACY, topSection: "allPlansExecution"},
};

// The subdocument holding the queryPlanner/executionStats sections: top-level for find and
// fully-lowered aggregate, or the first cursor stage for a classic pipeline.
function sectionsContainer(explain) {
    if (!explain.hasOwnProperty("stages")) {
        return explain;
    }
    const firstStage = explain.stages[0];
    return firstStage.$cursor || firstStage.$geoNearCursor;
}

// Whether the given inclusive section is present in the explain output.
function hasSection(explain, section) {
    switch (section) {
        case "queryPlanner":
            // Reuse the shared extractor, which validates and returns the section across all shapes.
            return getQueryPlanner(explain) !== undefined;
        case "executionStats":
            return sectionsContainer(explain).executionStats !== undefined;
        case "allPlansExecution": {
            const executionStats = sectionsContainer(explain).executionStats;
            return executionStats !== undefined && executionStats.allPlansExecution !== undefined;
        }
    }
    return false;
}

describe("explain V3 verbosity modes", function () {
    before(function () {
        const coll = db[collName];
        coll.drop();
        assert.commandWorked(
            coll.insert([
                {a: 1, b: 1},
                {a: 2, b: 2},
                {a: 2, b: 3},
            ]),
        );
        assert.commandWorked(coll.createIndex({a: 1}));
    });

    for (const {name, command} of testQueries) {
        for (const [verbosity, {version, topSection, topSectionByQuery}] of Object.entries(
            verbosityExpectations,
        )) {
            it(`${name} @ ${verbosity}`, function () {
                const explain = assert.commandWorked(db.runCommand({explain: command, verbosity}));

                // Version: exactly "3" for V3 modes; engine-determined "1"/"2" for legacy modes.
                if (version === V3) {
                    assert.eq(explain.explainVersion, V3, "unexpected explainVersion", {explain});
                } else {
                    assert.contains(explain.explainVersion, version, "unexpected explainVersion", {
                        explain,
                    });
                }

                // Sections are present up to and including 'topSection', and absent after it.
                const effectiveTopSection =
                    (topSectionByQuery && topSectionByQuery[name]) || topSection;
                const topIndex = SECTIONS.indexOf(effectiveTopSection);
                SECTIONS.forEach((section, index) => {
                    assert.eq(
                        hasSection(explain, section),
                        index <= topIndex,
                        `unexpected presence of section '${section}'`,
                        {explain},
                    );
                });
            });
        }
    }
});

// The stats-rich V3 modes' output shape on the find path (classic engine; the shape x ranker
// matrix lives in explain_plans_array.js and the executionStats parity in
// explain_exec_stats_parity.js).
describe("V3 stats-rich output shape (find path)", function () {
    const findCommand = {find: collName, filter: {a: 2}};
    let savedFrameworkControl;

    before(function () {
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        // Pin the classic engine: SBE-eligible queries keep legacy-shaped output until
        // SERVER-132033.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );

        const coll = db[collName];
        coll.drop();
        assert.commandWorked(
            coll.insert([
                {a: 1, b: 1},
                {a: 2, b: 2},
                {a: 2, b: 3},
            ]),
        );
        assert.commandWorked(coll.createIndex({a: 1}));
    });

    after(function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: savedFrameworkControl,
            }),
        );
    });

    // Strips the values that legitimately vary between two separate explain invocations of the
    // same query - planning/trial timing and yield bookkeeping - so the queryPlanner sections
    // can be compared for deep equality. Field *presence* is never normalized.
    const RUN_VARYING_FIELDS = new Set([
        "optimizationTimeMillis",
        "optimizationTimeMicros",
        "executionTimeMillisEstimate",
        "executionTimeMicros",
        "executionTimeNanos",
        "saveState",
        "restoreState",
        "needYield",
    ]);
    function normalizeRunVarying(value) {
        if (Array.isArray(value)) {
            return value.map(normalizeRunVarying);
        }
        if (typeof value === "object" && value !== null) {
            const out = {};
            for (const key of Object.keys(value)) {
                out[key] = RUN_VARYING_FIELDS.has(key)
                    ? "<normalized>"
                    : normalizeRunVarying(value[key]);
            }
            return out;
        }
        return value;
    }

    it("plannerStats renders plans[] and no legacy keys or execution sections", function () {
        const explain = assert.commandWorked(
            db.runCommand({explain: findCommand, verbosity: "plannerStats"}),
        );
        const queryPlanner = explain.queryPlanner;
        assert(Array.isArray(queryPlanner.plans), "missing queryPlanner.plans", {explain});
        assert(!queryPlanner.hasOwnProperty("winningPlan"), "unexpected winningPlan", {explain});
        assert(!queryPlanner.hasOwnProperty("rejectedPlans"), "unexpected rejectedPlans", {
            explain,
        });
        assert(!explain.hasOwnProperty("executionStats"), "unexpected executionStats", {explain});
    });

    it("execStats adds exactly the retained executionStats section", function () {
        const plannerStats = assert.commandWorked(
            db.runCommand({explain: findCommand, verbosity: "plannerStats"}),
        );
        const execStats = assert.commandWorked(
            db.runCommand({explain: findCommand, verbosity: "execStats"}),
        );

        // The queryPlanner section is identical across the two modes (modulo run-varying values).
        assert.docEq(
            normalizeRunVarying(plannerStats.queryPlanner),
            normalizeRunVarying(execStats.queryPlanner),
            "queryPlanner must be identical between plannerStats and execStats",
        );

        // execStats adds exactly the retained legacy section: winner executed, never an
        // allPlansExecution array (its content lives in queryPlanner.plans[]).
        assert(execStats.hasOwnProperty("executionStats"), "missing executionStats", {execStats});
        assert.eq(execStats.executionStats.executionSuccess, true, {execStats});
        assert(
            !execStats.executionStats.hasOwnProperty("allPlansExecution"),
            "unexpected allPlansExecution",
            {execStats},
        );
    });
});
