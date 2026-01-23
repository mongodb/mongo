/**
 * Tests $unionWith with views for extension stages. This includes:
 *  - $unionWith on a view, with extension stage in the view definition
 *  - $unionWith on a view, with extension stage in the subpipeline
 *  - $unionWith in a view definition, with extension stage in the subpipeline
 *  - Nested combinations of the above
 *
 * We test with a variety of extension stages, particularly those that desugar to ensure the
 * desugaring path is correct.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const testDb = db.getSiblingDB(jsTestName());
testDb.dropDatabase();
const collName = jsTestName();
const coll = testDb[collName];

const documents = [
    {_id: 0, x: 1, name: "apple"},
    {_id: 1, x: 2, name: "banana"},
    {_id: 2, x: 3, name: "cherry"},
    {_id: 3, x: 4, name: "date"},
    {_id: 4, x: 5, name: "elderberry"},
];
assert.commandWorked(coll.insertMany(documents));

const foreignCollName = collName + "_foreign";
const foreignColl = testDb[foreignCollName];

const foreignDocuments = [
    {_id: 0, x: 10, name: "fig"},
    {_id: 1, x: 20, name: "grape"},
    {_id: 2, x: 30, name: "honeydew"},
];
assert.commandWorked(foreignColl.insertMany(foreignDocuments));

// Helper to create a unique view name.
let viewCounter = 0;
function makeViewName(suffix) {
    return jsTestName() + "_view_" + viewCounter++ + "_" + suffix;
}

// Helper to drop views created during tests.
function dropView(viewName) {
    testDb[viewName].drop();
}

/**
 * Runs a standard test for $unionWith with views:
 *   1. Creates a view with the given pipeline (if viewPipeline is provided).
 *   2. Runs aggregation on the specified source.
 *   3. Validates result count.
 *   4. Validates main collection results.
 *   5. Validates unionWith results.
 *   6. Drops the view.
 *
 * @param {Object} opts - Test options.
 * @param {string} opts.desc - Test description.
 * @param {Array} [opts.viewPipeline] - Pipeline for the view definition.
 * @param {string} [opts.viewSourceColl=collName] - Collection the view is based on.
 * @param {boolean} [opts.aggOnView=false] - If true, run aggregate on the created view.
 * @param {string} [opts.aggSourceColl=collName] - Collection to run aggregate on (ignored if aggOnView=true).
 * @param {Array|Function} [opts.pipeline=[]] - Pipeline to run, or function (viewName) => Array.
 * @param {Array} opts.expectedMainDocs - Expected docs from main part of results.
 * @param {Array} [opts.expectedUnionWithDocs=[]] - Expected docs from unionWith part.
 * @param {boolean} [opts.compareOrdered=false] - Use ordered comparison for unionWith docs.
 * @param {Function} [opts.unionWithDocComparator=null] - Custom comparator: (actual, expected) => void.
 */
function runTest({
    desc,
    viewPipeline = null,
    viewSourceColl = collName,
    aggOnView = false,
    aggSourceColl = collName,
    pipeline = [],
    expectedMainDocs,
    expectedUnionWithDocs = [],
    compareOrdered = false,
    unionWithDocComparator = null,
}) {
    jsTest.log.info("Testing " + desc);

    let viewName = null;
    if (viewPipeline) {
        viewName = makeViewName(desc.replace(/[^a-zA-Z0-9]/g, "_").slice(0, 30));
        assert.commandWorked(testDb.createView(viewName, viewSourceColl, viewPipeline));
    }

    // Build the pipeline - it can be a function that takes viewName, or a static array.
    const actualPipeline = typeof pipeline === "function" ? pipeline(viewName) : pipeline;

    // Determine which collection to aggregate on.
    const aggColl = aggOnView ? testDb[viewName] : testDb[aggSourceColl];
    const results = aggColl.aggregate(actualPipeline).toArray();

    assert.eq(results.length, expectedMainDocs.length + expectedUnionWithDocs.length, results);
    assertArrayEq({actual: results.slice(0, expectedMainDocs.length), expected: expectedMainDocs});

    if (expectedUnionWithDocs.length > 0) {
        const unionWithResults = results.slice(expectedMainDocs.length);
        if (unionWithDocComparator) {
            unionWithDocComparator(unionWithResults, expectedUnionWithDocs);
        } else if (compareOrdered) {
            assert.eq(unionWithResults, expectedUnionWithDocs, results);
        } else {
            assertArrayEq({actual: unionWithResults, expected: expectedUnionWithDocs});
        }
    }

    if (viewName) {
        dropView(viewName);
    }
}

/**
 * Wrapper for running tests where we aggregate on a collection with $unionWith pointing to a view.
 * This is the most common pattern in these tests.
 *
 * @param {Object} opts - Test options.
 * @param {string} opts.desc - Test description.
 * @param {Array} opts.viewPipeline - Pipeline for the view definition.
 * @param {string} [opts.viewSourceColl=collName] - Collection the view is based on.
 * @param {Array} [opts.unionWithPipeline=[]] - Subpipeline for $unionWith.
 * @param {Array} [opts.mainPipeline=[]] - Pipeline stages before $unionWith.
 * @param {Array} opts.expectedViewDocs - Expected documents from the view.
 * @param {Array} [opts.expectedMainDocs=documents] - Expected docs from main pipeline.
 * @param {boolean} [opts.compareOrdered=false] - Use ordered comparison for view docs.
 * @param {Function} [opts.viewDocComparator=null] - Custom comparator: (actual, expected) => void.
 */
function runUnionWithOnViewTest({
    desc,
    viewPipeline,
    viewSourceColl = collName,
    unionWithPipeline = [],
    mainPipeline = [],
    expectedViewDocs,
    expectedMainDocs = documents,
    compareOrdered = false,
    viewDocComparator = null,
}) {
    runTest({
        desc,
        viewPipeline,
        viewSourceColl,
        aggOnView: false,
        aggSourceColl: collName,
        pipeline: (viewName) => [...mainPipeline, {$unionWith: {coll: viewName, pipeline: unionWithPipeline}}],
        expectedMainDocs,
        expectedUnionWithDocs: expectedViewDocs,
        compareOrdered,
        unionWithDocComparator: viewDocComparator,
    });
}

// =============================================================================
// $unionWith on a view with extension stage in the view definition
// =============================================================================
runUnionWithOnViewTest({
    desc: "$unionWith on view with $testBar extension in definition",
    viewPipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 3}}}],
    expectedViewDocs: documents.filter((d) => d.x <= 3),
});

runUnionWithOnViewTest({
    desc: "$unionWith on view with $testBar extension, with additional subpipeline",
    viewPipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 3}}}],
    unionWithPipeline: [{$match: {name: {$in: ["apple", "cherry"]}}}],
    expectedViewDocs: documents.filter((d) => d.x <= 3 && ["apple", "cherry"].includes(d.name)),
});

runUnionWithOnViewTest({
    desc: "$unionWith on view with $matchTopN (desugar) extension in definition",
    viewPipeline: [{$matchTopN: {filter: {x: {$gte: 2}}, sort: {x: -1}, limit: 3}}],
    expectedViewDocs: [
        {_id: 4, x: 5, name: "elderberry"},
        {_id: 3, x: 4, name: "date"},
        {_id: 2, x: 3, name: "cherry"},
    ],
    compareOrdered: true,
});

runUnionWithOnViewTest({
    desc: "$unionWith on view with $toast (source) extension in definition",
    viewPipeline: [{$toast: {temp: 350.0, numSlices: 3}}],
    expectedViewDocs: [
        {slice: 0, isBurnt: false},
        {slice: 1, isBurnt: false},
        {slice: 2, isBurnt: false},
    ],
    viewDocComparator: (actual, expected) => assert.docEq(expected, actual),
});

runUnionWithOnViewTest({
    desc: "$unionWith on view with $readNDocuments (desugar with idLookup) in definition",
    viewPipeline: [{$readNDocuments: {numDocs: 2}}],
    expectedViewDocs: documents.slice(0, 2),
    viewDocComparator: (actual, expected) => assert.sameMembers(actual, expected),
});

// =============================================================================
// $unionWith on a view with extension stage in the subpipeline
// =============================================================================
runUnionWithOnViewTest({
    desc: "$unionWith on view with $testBar extension in subpipeline",
    viewPipeline: [{$match: {x: {$gte: 2}}}],
    unionWithPipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 3}}}],
    expectedViewDocs: documents.filter((d) => d.x >= 2 && d.x <= 3),
});

runUnionWithOnViewTest({
    desc: "$unionWith on view with $matchTopN (desugar) in subpipeline",
    viewPipeline: [{$addFields: {doubled: {$multiply: ["$x", 2]}}}],
    unionWithPipeline: [{$matchTopN: {filter: {}, sort: {doubled: -1}, limit: 2}}],
    expectedViewDocs: [
        {_id: 4, x: 5, name: "elderberry", doubled: 10},
        {_id: 3, x: 4, name: "date", doubled: 8},
    ],
    compareOrdered: true,
});

runUnionWithOnViewTest({
    desc: "$unionWith on view with $extensionLimit in subpipeline",
    viewPipeline: [{$sort: {_id: 1}}],
    unionWithPipeline: [{$extensionLimit: 3}],
    expectedViewDocs: documents.slice(0, 3),
    compareOrdered: true,
});

// =============================================================================
// $unionWith in a view definition with extension stage in subpipeline.
// These tests query the view directly using aggOnView: true.
// =============================================================================
runTest({
    desc: "$unionWith in view definition with $testBar in subpipeline",
    viewPipeline: [
        {$match: {x: {$lte: 2}}},
        {$unionWith: {coll: foreignCollName, pipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 20}}}]}},
    ],
    aggOnView: true,
    pipeline: [],
    expectedMainDocs: documents.filter((d) => d.x <= 2),
    expectedUnionWithDocs: foreignDocuments.filter((d) => d.x <= 20),
});

// Test querying through the view with an additional pipeline that filters results.
runTest({
    desc: "$unionWith in view definition with $testBar, queried with additional filter",
    viewPipeline: [
        {$match: {x: {$lte: 2}}},
        {$unionWith: {coll: foreignCollName, pipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 20}}}]}},
    ],
    aggOnView: true,
    pipeline: [{$match: {x: {$gte: 10}}}],
    // The filter x >= 10 excludes all main docs (x <= 2) and keeps foreign docs with x <= 20.
    expectedMainDocs: [],
    expectedUnionWithDocs: foreignDocuments.filter((d) => d.x <= 20),
});

runTest({
    desc: "$unionWith in view definition with $matchTopN (desugar) in subpipeline",
    viewPipeline: [
        {$project: {_id: 1, x: 1, name: 1}},
        {$unionWith: {coll: foreignCollName, pipeline: [{$matchTopN: {filter: {}, sort: {x: 1}, limit: 2}}]}},
    ],
    aggOnView: true,
    pipeline: [],
    expectedMainDocs: documents.map((d) => ({_id: d._id, x: d.x, name: d.name})),
    // Top 2 foreign docs by x ascending: x=10 (fig), x=20 (grape).
    expectedUnionWithDocs: [
        {_id: 0, x: 10, name: "fig"},
        {_id: 1, x: 20, name: "grape"},
    ],
    compareOrdered: true,
});

runTest({
    desc: "$unionWith in view definition with $toast source extension in subpipeline",
    viewPipeline: [
        {$sort: {_id: 1}},
        {$limit: 2},
        {$unionWith: {coll: collName, pipeline: [{$toast: {temp: 400.0, numSlices: 2}}]}},
    ],
    aggOnView: true,
    pipeline: [],
    expectedMainDocs: documents.slice(0, 2),
    expectedUnionWithDocs: [
        {slice: 0, isBurnt: false},
        {slice: 1, isBurnt: false},
    ],
    unionWithDocComparator: (actual, expected) => assert.docEq(expected, actual),
});

// =============================================================================
// Nested combinations
// These tests involve multiple views or deeply nested $unionWith.
// =============================================================================

// Nested $unionWith: extension in outer view definition, extension in inner subpipeline.
// View docs (x <= 3) are combined with nested union docs (foreign name === "fig").
{
    const expectedOuterViewDocs = documents.filter((d) => d.x <= 3);
    const expectedNestedDocs = foreignDocuments.filter((d) => d.name === "fig");

    runUnionWithOnViewTest({
        desc: "nested $unionWith: extension in outer view, extension in inner subpipeline",
        viewPipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 3}}}],
        unionWithPipeline: [
            {$unionWith: {coll: foreignCollName, pipeline: [{$testBar: {noop: true}}, {$match: {name: "fig"}}]}},
        ],
        expectedViewDocs: [...expectedOuterViewDocs, ...expectedNestedDocs],
    });
}

// Nested $unionWith with multiple views containing extensions.
// viewB must be created first since it's referenced in the subpipeline.
{
    const expectedViewBDocs = foreignDocuments.filter((d) => d.name === "grape");
    const viewBName = makeViewName("nested_viewB");
    assert.commandWorked(
        testDb.createView(viewBName, foreignCollName, [{$testBar: {noop: true}}, {$match: {name: "grape"}}]),
    );

    const expectedViewADocs = documents.filter((d) => d.x <= 2);

    runUnionWithOnViewTest({
        desc: "nested $unionWith with multiple views containing extensions",
        viewPipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 2}}}],
        mainPipeline: [{$match: {_id: 0}}],
        unionWithPipeline: [{$unionWith: viewBName}],
        expectedMainDocs: documents.filter((d) => d._id === 0),
        expectedViewDocs: [...expectedViewADocs, ...expectedViewBDocs],
    });

    dropView(viewBName);
}

// View definition has $unionWith, used in another $unionWith with extension subpipeline.
// The view contains main docs (x <= 2) unioned with all foreign docs. The subpipeline filters
// to docs where x >= 10 (only foreign docs) or name is "apple".
{
    const expectedInnerViewDocs = [
        ...documents.filter((d) => d.x <= 2 && d.name === "apple"),
        ...foreignDocuments.filter((d) => d.x >= 10),
    ];

    runUnionWithOnViewTest({
        desc: "view with $unionWith definition, used in $unionWith with extension subpipeline",
        viewPipeline: [{$match: {x: {$lte: 2}}}, {$unionWith: foreignCollName}],
        mainPipeline: [{$match: {_id: 0}}],
        unionWithPipeline: [{$testBar: {noop: true}}, {$match: {$or: [{x: {$gte: 10}}, {name: "apple"}]}}],
        expectedMainDocs: documents.filter((d) => d._id === 0),
        expectedViewDocs: expectedInnerViewDocs,
    });
}

// Deeply nested $unionWith with multiple extensions at different levels.
// level3 view must exist before level2 can reference it.
{
    const level3ViewName = makeViewName("level3");
    assert.commandWorked(
        testDb.createView(level3ViewName, foreignCollName, [{$testBar: {noop: true}}, {$sort: {_id: 1}}, {$limit: 1}]),
    );

    // level2 view: docs with x >= 3 (3 docs) + 1 foreign doc from level3.
    const level2Pipeline = [{$testBar: {noop: true}}, {$match: {x: {$gte: 3}}}, {$unionWith: level3ViewName}];

    runUnionWithOnViewTest({
        desc: "deeply nested $unionWith with multiple extensions at different levels",
        viewPipeline: level2Pipeline,
        mainPipeline: [{$sort: {_id: 1}}, {$limit: 2}],
        unionWithPipeline: [{$extensionLimit: 5}],
        expectedMainDocs: documents.slice(0, 2),
        // min(5, 3 docs with x>=3 + 1 foreign doc) = 4 docs.
        expectedViewDocs: [...documents.filter((d) => d.x >= 3), ...foreignDocuments.slice(0, 1)],
    });

    dropView(level3ViewName);
}

// Triple nested $unionWith where each level has extensions.
{
    const expectedView1Docs = documents.filter((d) => d.x <= 2);
    // $matchTopN with sort x:1 and limit 1 returns the foreign doc with smallest x (x=10, "fig").
    const expectedForeignDocs = [{_id: 0, x: 10, name: "fig"}];

    runUnionWithOnViewTest({
        desc: "triple nested $unionWith where each level has extensions",
        viewPipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 2}}}],
        mainPipeline: [{$match: {_id: 0}}],
        unionWithPipeline: [
            {$testBar: {noop: true}},
            {$unionWith: {coll: foreignCollName, pipeline: [{$matchTopN: {filter: {}, sort: {x: 1}, limit: 1}}]}},
        ],
        expectedMainDocs: documents.filter((d) => d._id === 0),
        expectedViewDocs: [...expectedView1Docs, ...expectedForeignDocs],
    });
}

// =============================================================================
// Edge cases and special scenarios
// =============================================================================
runUnionWithOnViewTest({
    desc: "$unionWith on view with extension when results are empty",
    viewPipeline: [{$testBar: {noop: true}}, {$match: {x: {$gt: 1000}}}],
    expectedViewDocs: [],
});

runUnionWithOnViewTest({
    desc: "$unionWith on foreign collection through view with extension",
    viewPipeline: [{$testBar: {noop: true}}, {$addFields: {source: "foreign"}}],
    viewSourceColl: foreignCollName,
    mainPipeline: [{$addFields: {source: "main"}}],
    expectedMainDocs: documents.map((d) => Object.assign({}, d, {source: "main"})),
    expectedViewDocs: foreignDocuments.map((d) => Object.assign({}, d, {source: "foreign"})),
});

// View chain test: view2 is based on view1, both with extensions.
// view1 filters to x >= 3, view2 further filters to x <= 4, so we get docs where 3 <= x <= 4.
{
    const view1Name = makeViewName("chain_v1");
    assert.commandWorked(testDb.createView(view1Name, collName, [{$testBar: {noop: true}}, {$match: {x: {$gte: 3}}}]));

    runUnionWithOnViewTest({
        desc: "view chain where each view has extension",
        viewPipeline: [{$testBar: {noop: true}}, {$match: {x: {$lte: 4}}}],
        viewSourceColl: view1Name,
        expectedViewDocs: documents.filter((d) => d.x >= 3 && d.x <= 4),
    });

    dropView(view1Name);
}

// =============================================================================
// Test with desugar stages to ensure desugaring path is correct
// =============================================================================
runUnionWithOnViewTest({
    desc: "desugar extension ($readNDocuments) in view definition, then $unionWith with subpipeline",
    viewPipeline: [{$readNDocuments: {numDocs: 3}}],
    mainPipeline: [{$sort: {_id: 1}}, {$limit: 2}],
    unionWithPipeline: [{$sort: {_id: 1}}],
    expectedMainDocs: documents.slice(0, 2),
    expectedViewDocs: documents.slice(0, 3),
    viewDocComparator: (actual, expected) => assert.sameMembers(actual, expected),
});

// Desugar extension in subpipeline of $unionWith on a view.
// Top 2 foreign docs by x descending: x=30 (honeydew), x=20 (grape).
runUnionWithOnViewTest({
    desc: "desugar extension in subpipeline of $unionWith on a view",
    viewPipeline: [{$addFields: {type: "foreign"}}],
    viewSourceColl: foreignCollName,
    mainPipeline: [{$addFields: {type: "main"}}],
    unionWithPipeline: [{$matchTopN: {filter: {}, sort: {x: -1}, limit: 2}}],
    expectedMainDocs: documents.map((d) => Object.assign({}, d, {type: "main"})),
    expectedViewDocs: [
        {_id: 2, x: 30, name: "honeydew", type: "foreign"},
        {_id: 1, x: 20, name: "grape", type: "foreign"},
    ],
    compareOrdered: true,
});

/**
 * Extract the explain output for a $unionWith subpipeline and validate that the expected
 * extension stage appears with correct parameters.
 */
function verifyExtensionStageInUnionWithSubpipeline(explainOutput, expectedStage, expectedParams, verbosity) {
    const unionWithStages = getAggPlanStages(explainOutput, "$unionWith");
    assert.gt(unionWithStages.length, 0, `Expected $unionWith in explain output: ${tojson(explainOutput)}`);

    for (const unionWithStage of unionWithStages) {
        const subpipelineExplain = unionWithStage["$unionWith"]["pipeline"];

        // A $unionWith subpipeline looks the same as a top-level pipeline in the explain output,
        // just nested differently. Present it like a top-level pipeline for getAggPlanStages.
        const subpipelineForSearch = subpipelineExplain.hasOwnProperty("shards")
            ? subpipelineExplain
            : {stages: subpipelineExplain};

        const extensionStages = getAggPlanStages(subpipelineForSearch, expectedStage);
        assert.gt(
            extensionStages.length,
            0,
            `Expected ${expectedStage} in subpipeline explain output: ${tojson(subpipelineExplain)}`,
        );

        const stageOutput = extensionStages[0];
        assert.eq(stageOutput[expectedStage], expectedParams, `Stage params mismatch: ${tojson(stageOutput)}`);

        // Verify execution stats are populated for non-queryPlanner verbosity by checking for a
        // metric that the $explain extension itself provides.
        if (verbosity !== "queryPlanner") {
            assert.eq(
                stageOutput.execMetricField,
                "execMetricValue",
                `Missing extension-provided execMetricField: ${tojson(stageOutput)}`,
            );
        }
    }
}

// Skip explain tests on mongos as the explain output structure differs significantly for sharded
// collections and the $unionWith explain is not directly comparable.
if (!FixtureHelpers.isMongos(db)) {
    (function testExplain_ExtensionInViewDefinition() {
        jsTest.log.info("Testing explain: extension stage in view definition populates explain output");

        // Use $explain extension which has well-defined explain serialization behavior.
        const viewName = makeViewName("explain_ext_in_view_def");
        assert.commandWorked(testDb.createView(viewName, collName, [{$explain: {input: "fromView"}}]));

        for (const verbosity of ["queryPlanner", "executionStats"]) {
            const explain = coll.explain(verbosity).aggregate([{$unionWith: viewName}]);
            verifyExtensionStageInUnionWithSubpipeline(explain, "$explain", {input: "fromView", verbosity}, verbosity);
        }

        dropView(viewName);
    })();

    (function testExplain_ExtensionInSubpipelineOnView() {
        jsTest.log.info("Testing explain: extension stage in subpipeline on view populates explain output");

        // View has no extension, but the $unionWith subpipeline does.
        const viewName = makeViewName("explain_ext_in_subpipeline");
        assert.commandWorked(testDb.createView(viewName, collName, [{$match: {x: {$gte: 1}}}]));

        const pipeline = [{$unionWith: {coll: viewName, pipeline: [{$explain: {input: "fromSubpipeline"}}]}}];
        for (const verbosity of ["queryPlanner", "executionStats"]) {
            const explain = coll.explain(verbosity).aggregate(pipeline);
            verifyExtensionStageInUnionWithSubpipeline(
                explain,
                "$explain",
                {input: "fromSubpipeline", verbosity},
                verbosity,
            );
        }

        dropView(viewName);
    })();
}

jsTest.log.info("All $unionWith with views extension tests passed!");
