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
 * Each V3 mode reports "explainVersion: '3'". On the find path, plannerChoice and
 * plannerStats/execStats all render the real V3 queryPlanner (a "plans" array), differing only in
 * which statistics the plans carry: plannerChoice carries none, plannerStats adds the ranking
 * statistics without executing the query, and execStats adds exactly the retained legacy
 * kExecStats section (never an allPlansExecution array - that content lives in
 * queryPlanner.plans[]). planSummary remains legacy-delegated (-> queryPlanner) until
 * SERVER-133235.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getAggPlanStage,
    getQueryPlanner,
    isV3QueryPlanner,
    normalizeRunVarying,
} from "jstests/libs/query/analyze_plan.js";

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
        // A $unionWith followed by a $match: the trailing $match is duplicated across the union.
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
const V3 = "3";
const LEGACY = ["1", "2"]; // engine-determined (Classic / SBE)
const verbosityExpectations = {
    // V3 modes.
    planSummary: {version: V3, topSection: "queryPlanner"},
    plannerChoice: {version: V3, topSection: "queryPlanner"},
    // No execution sections at all: the trial statistics live in queryPlanner.plans[] and neither
    // the query nor the pipeline is executed.
    plannerStats: {version: V3, topSection: "queryPlanner"},
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
        for (const [verbosity, {version, topSection}] of Object.entries(verbosityExpectations)) {
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
                const topIndex = SECTIONS.indexOf(topSection);
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

// The stats-rich V3 modes' output shape on the find path, run once per execution engine (the shape x
// ranker matrix lives in explain_plans_array.js and the executionStats parity in
// explain_exec_stats_parity.js).
for (const engine of ["forceClassicEngine", "trySbeEngine"]) {
    describe(`V3 stats-rich output shape (find path, ${engine})`, function () {
        const findCommand = {find: collName, filter: {a: 2}};
        let savedFrameworkControl;

        before(function () {
            savedFrameworkControl = assert.commandWorked(
                db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
            ).internalQueryFrameworkControl;
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryFrameworkControl: engine}),
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

        it("plannerChoice renders plans[] and no execution sections", function () {
            const explain = assert.commandWorked(
                db.runCommand({explain: findCommand, verbosity: "plannerChoice"}),
            );
            const queryPlanner = explain.queryPlanner;
            assert(Array.isArray(queryPlanner.plans), "missing queryPlanner.plans", {explain});
            assert(!queryPlanner.hasOwnProperty("winningPlan"), "unexpected winningPlan", {
                explain,
            });
            assert(!queryPlanner.hasOwnProperty("rejectedPlans"), "unexpected rejectedPlans", {
                explain,
            });
            assert(!explain.hasOwnProperty("executionStats"), "unexpected executionStats", {
                explain,
            });
        });

        it("plannerStats renders plans[] and no legacy keys or execution sections", function () {
            const explain = assert.commandWorked(
                db.runCommand({explain: findCommand, verbosity: "plannerStats"}),
            );
            const queryPlanner = explain.queryPlanner;
            assert(Array.isArray(queryPlanner.plans), "missing queryPlanner.plans", {explain});
            assert(!queryPlanner.hasOwnProperty("winningPlan"), "unexpected winningPlan", {
                explain,
            });
            assert(!queryPlanner.hasOwnProperty("rejectedPlans"), "unexpected rejectedPlans", {
                explain,
            });
            assert(!explain.hasOwnProperty("executionStats"), "unexpected executionStats", {
                explain,
            });
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
            assert(execStats.hasOwnProperty("executionStats"), "missing executionStats", {
                execStats,
            });
            assert.eq(execStats.executionStats.executionSuccess, true, {execStats});
            assert(
                !execStats.executionStats.hasOwnProperty("allPlansExecution"),
                "unexpected allPlansExecution",
                {execStats},
            );
        });
    });
}

describe("V3 for aggregation pipelines", function () {
    // $_internalInhibitOptimization keeps this a classic DocumentSource pipeline.
    const aggCommand = {
        aggregate: collName,
        pipeline: [
            {$match: {a: {$lt: 2}}},
            {$_internalInhibitOptimization: {}},
            {$group: {_id: "$a", c: {$sum: 1}}},
        ],
        cursor: {},
    };
    let savedFrameworkControl;

    before(function () {
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        // TODO SERVER-132033 remove once SBE-eligible plans are supported in V3.
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

    for (const verbosity of ["plannerChoice", "plannerStats", "execStats"]) {
        it(`${verbosity} renders the V3 plans[] under $cursor`, function () {
            const explain = assert.commandWorked(db.runCommand({explain: aggCommand, verbosity}));
            assert.eq(explain.explainVersion, "3", "unexpected explainVersion", {explain});

            const queryPlanner = getQueryPlanner(explain);
            assert(isV3QueryPlanner(queryPlanner), "expected the V3 queryPlanner shape", {explain});
            assert(Array.isArray(queryPlanner.plans), "missing queryPlanner.plans", {explain});
            assert(!queryPlanner.hasOwnProperty("rejectedPlans"), "unexpected rejectedPlans", {
                explain,
            });

            const leaf = getAggPlanStage(explain, "IXSCAN") || getAggPlanStage(explain, "COLLSCAN");
            assert(leaf, "expected the query layer's access stage to be reachable", {explain});
        });
    }

    it("plannerChoice and plannerStats do not execute the pipeline", function () {
        for (const verbosity of ["plannerChoice", "plannerStats"]) {
            const explain = assert.commandWorked(db.runCommand({explain: aggCommand, verbosity}));
            assert(
                !sectionsContainer(explain).hasOwnProperty("executionStats"),
                `unexpected executionStats at ${verbosity}`,
                {explain},
            );
            for (const stage of explain.stages) {
                assert(
                    !stage.hasOwnProperty("nReturned"),
                    `unexpected per-stage execution stats at ${verbosity}`,
                    {explain},
                );
            }
        }
    });

    it("execStats adds the executionStats section under $cursor", function () {
        const explain = assert.commandWorked(
            db.runCommand({explain: aggCommand, verbosity: "execStats"}),
        );
        const executionStats = sectionsContainer(explain).executionStats;
        assert(executionStats, "missing executionStats", {explain});
        assert.eq(executionStats.executionSuccess, true, {explain});
        assert(
            !executionStats.hasOwnProperty("allPlansExecution"),
            "unexpected allPlansExecution",
            {explain},
        );
    });
});

describe("V3 for aggregation pipelines", function () {
    // $_internalInhibitOptimization keeps this a classic DocumentSource pipeline.
    const aggCommand = {
        aggregate: collName,
        pipeline: [
            {$match: {a: {$lt: 2}}},
            {$_internalInhibitOptimization: {}},
            {$group: {_id: "$a", c: {$sum: 1}}},
        ],
        cursor: {},
    };
    let savedFrameworkControl;

    before(function () {
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        // TODO SERVER-132033 remove once SBE-eligible plans are supported in V3.
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

    for (const verbosity of ["plannerChoice", "plannerStats", "execStats"]) {
        it(`${verbosity} renders the V3 plans[] under $cursor`, function () {
            const explain = assert.commandWorked(db.runCommand({explain: aggCommand, verbosity}));
            assert.eq(explain.explainVersion, "3", "unexpected explainVersion", {explain});

            const queryPlanner = getQueryPlanner(explain);
            assert(isV3QueryPlanner(queryPlanner), "expected the V3 queryPlanner shape", {explain});
            assert(Array.isArray(queryPlanner.plans), "missing queryPlanner.plans", {explain});
            assert(!queryPlanner.hasOwnProperty("rejectedPlans"), "unexpected rejectedPlans", {
                explain,
            });

            const leaf = getAggPlanStage(explain, "IXSCAN") || getAggPlanStage(explain, "COLLSCAN");
            assert(leaf, "expected the query layer's access stage to be reachable", {explain});
        });
    }

    it("plannerChoice and plannerStats do not execute the pipeline", function () {
        for (const verbosity of ["plannerChoice", "plannerStats"]) {
            const explain = assert.commandWorked(db.runCommand({explain: aggCommand, verbosity}));
            assert(
                !sectionsContainer(explain).hasOwnProperty("executionStats"),
                `unexpected executionStats at ${verbosity}`,
                {explain},
            );
            for (const stage of explain.stages) {
                assert(
                    !stage.hasOwnProperty("nReturned"),
                    `unexpected per-stage execution stats at ${verbosity}`,
                    {explain},
                );
            }
        }
    });

    it("execStats adds the executionStats section under $cursor", function () {
        const explain = assert.commandWorked(
            db.runCommand({explain: aggCommand, verbosity: "execStats"}),
        );
        const executionStats = sectionsContainer(explain).executionStats;
        assert(executionStats, "missing executionStats", {explain});
        assert.eq(executionStats.executionSuccess, true, {explain});
        assert(
            !executionStats.hasOwnProperty("allPlansExecution"),
            "unexpected allPlansExecution",
            {explain},
        );
    });
});
