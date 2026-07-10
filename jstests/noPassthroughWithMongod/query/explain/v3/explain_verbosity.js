/**
 * Tests the new "version 3" (V3) explain verbosity modes: planSummary, plannerChoice, plannerStats,
 * execStats.
 *
 * Data-driven: `testQueries` lists the queries as plain command documents, and
 * `verbosityExpectations` maps each verbosity to the explain version it reports and the highest
 * explain section it should produce. Sections are inclusive along the legacy ladder
 * queryPlanner ⊂ executionStats ⊂ allPlansExecution: a verbosity that reaches a section includes
 * that section and every section before it, and none after. The test runs every query under every
 * verbosity.
 *
 * Current skeleton mapping (SERVER-130403; the real V3 output format is SERVER-130529). Each V3
 * mode reports "explainVersion: '3'" but reuses the nearest legacy verbosity's output:
 *   planSummary, plannerChoice -> queryPlanner
 *   plannerStats               -> allPlansExecution
 *   execStats                  -> executionStats   (execStats == legacy executionStats by definition)
 */
import {before, describe, it} from "jstests/libs/mochalite.js";
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
        // gated on an ordinal comparison of the ExpressionContext verbosity
        // (document_source_union_with.cpp, "getExplain() >= kExecStats"). Since the V3 modes sort
        // >= kExecStats, that branch runs even for the planner-only modes planSummary/plannerChoice.
        // This case guards that the planner-only modes still emit no execution content (the ordinal
        // comparison must not leak execution sections into V3 planner-only output).
        // TODO SERVER-130810 / SERVER-130812 once the aggregate path threads the real V3 verbosity
        // and the ordinal comparisons are replaced with semantic predicates.
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
    plannerStats: {version: V3, topSection: "allPlansExecution"},
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
