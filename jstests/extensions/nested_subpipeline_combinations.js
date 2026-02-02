/**
 * Tests complex nested subpipeline combinations with views and extensions.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */

const testDb = db.getSiblingDB(jsTestName());
testDb.dropDatabase();

const collName = jsTestName();
const coll = testDb[collName];

const documents = [
    {_id: 0, x: 1, foreignKey: 10, category: "A"},
    {_id: 1, x: 2, foreignKey: 20, category: "B"},
    {_id: 2, x: 3, foreignKey: 30, category: "A"},
    {_id: 3, x: 4, foreignKey: 10, category: "B"},
    {_id: 4, x: 5, foreignKey: 20, category: "A"},
];
assert.commandWorked(coll.insertMany(documents));

const foreignCollName = collName + "_foreign";
const foreignColl = testDb[foreignCollName];

const foreignDocuments = [
    {_id: 10, value: "alpha", type: "X"},
    {_id: 20, value: "beta", type: "Y"},
    {_id: 30, value: "gamma", type: "X"},
];
assert.commandWorked(foreignColl.insertMany(foreignDocuments));

const graphCollName = collName + "_graph";
const graphColl = testDb[graphCollName];

const graphDocuments = [
    {_id: 1, name: "root", parent: null},
    {_id: 2, name: "child1", parent: "root"},
    {_id: 3, name: "child2", parent: "root"},
    {_id: 4, name: "grandchild1", parent: "child1"},
    {_id: 5, name: "grandchild2", parent: "child2"},
];
assert.commandWorked(graphColl.insertMany(graphDocuments));

// Helper to create unique view names.
let viewCounter = 0;
function makeViewName(suffix) {
    return jsTestName() + "_view_" + viewCounter++ + "_" + suffix;
}

function dropView(viewName) {
    testDb[viewName].drop();
}

/**
 * Creates a chain of views and returns an object with view names for cleanup.
 * @param {Array} viewSpecs - Array of {suffix, sourceColl, pipeline} objects.
 *                            If sourceColl is null, uses previous view in chain.
 * @returns {Object} Object with viewNames array and topViewName.
 */
function createViewChain(viewSpecs) {
    const viewNames = [];
    let prevView = null;

    for (const spec of viewSpecs) {
        const viewName = makeViewName(spec.suffix);
        const source = spec.sourceColl || prevView;
        assert.commandWorked(testDb.createView(viewName, source, spec.pipeline));
        viewNames.push(viewName);
        prevView = viewName;
    }

    return {
        viewNames,
        topViewName: viewNames[viewNames.length - 1],
        cleanup: function () {
            for (let i = viewNames.length - 1; i >= 0; i--) {
                dropView(viewNames[i]);
            }
        },
    };
}

// =============================================================================
// Multiple levels of view nesting with extensions at different levels
// =============================================================================
{
    const chain = createViewChain([
        {suffix: "level1", sourceColl: collName, pipeline: [{$testBar: {noop: true}}]},
        {suffix: "level2", sourceColl: null, pipeline: [{$addFields: {level2: true}}]},
        {suffix: "level3", sourceColl: null, pipeline: [{$testBar: {noop: true}}, {$addFields: {level3: true}}]},
        {suffix: "level4", sourceColl: null, pipeline: [{$addFields: {level4: true}}]},
    ]);

    // Query the deepest level.
    const results = testDb[chain.topViewName].aggregate([{$sort: {_id: 1}}, {$limit: 2}]).toArray();
    assert.eq(results.length, 2, results);
    results.forEach((doc) => {
        assert.eq(doc.level2, true, doc);
        assert.eq(doc.level3, true, doc);
        assert.eq(doc.level4, true, doc);
    });

    chain.cleanup();
}

// =============================================================================
// $unionWith -> $unionWith -> view with extension
// =============================================================================
{
    const viewName = makeViewName("double_unionwith_target");
    assert.commandWorked(
        testDb.createView(viewName, foreignCollName, [{$testBar: {noop: true}}, {$match: {type: "X"}}]),
    );

    const pipeline = [
        {$sort: {_id: 1}},
        {$limit: 1},
        {
            $unionWith: {
                coll: collName,
                pipeline: [{$sort: {_id: 1}}, {$limit: 1}, {$unionWith: viewName}],
            },
        },
    ];

    const results = coll.aggregate(pipeline).toArray();
    // 1 from main + 1 from first unionWith + 2 from nested unionWith (type X docs).
    assert.eq(results.length, 4, results);

    dropView(viewName);
}

// =============================================================================
// Mixed extension types at different view nesting levels
// =============================================================================
{
    // Tests: transform extension at base, desugaring extension in middle, non-desugaring at top
    const chain = createViewChain([
        {suffix: "mixed_level1_transform", sourceColl: collName, pipeline: [{$sort: {_id: 1}}, {$extensionLimit: 4}]},
        {
            suffix: "mixed_level2_desugar",
            sourceColl: null,
            pipeline: [
                {$addFields: {fromLevel2: true}},
                {$matchTopN: {filter: {x: {$gte: 2}}, sort: {x: -1}, limit: 3}},
            ],
        },
        {
            suffix: "mixed_level3_noop",
            sourceColl: null,
            pipeline: [{$testBar: {noop: true}}, {$addFields: {fromLevel3: true}}],
        },
    ]);

    const results = testDb[chain.topViewName].aggregate([{$sort: {x: -1}}]).toArray();

    // Level 1 limits to 4 docs (ids 0,1,2,3).
    // Level 2 filters x >= 2 from those 4 (ids 1,2,3), sorts by x desc, limits to 3.
    // All 3 should pass through.
    assert.eq(results.length, 3, results);
    results.forEach((doc) => {
        assert.eq(doc.fromLevel2, true, doc);
        assert.eq(doc.fromLevel3, true, doc);
        assert.gte(doc.x, 2, doc);
    });

    chain.cleanup();
}

// =============================================================================
// $graphLookup + $unionWith combination with views containing extensions
// =============================================================================
{
    // View for $graphLookup with extension.
    const graphViewName = makeViewName("graph_ext");
    assert.commandWorked(
        testDb.createView(graphViewName, graphCollName, [
            {$testBar: {noop: true}},
            {$addFields: {fromGraphView: true}},
        ]),
    );

    // View for $unionWith with different extension.
    const unionViewName = makeViewName("union_ext");
    assert.commandWorked(
        testDb.createView(unionViewName, foreignCollName, [
            {$matchTopN: {filter: {type: "X"}, sort: {_id: 1}, limit: 2}},
            {$addFields: {fromUnionView: true}},
        ]),
    );

    const pipeline = [
        {$match: {_id: 0}},
        {
            $graphLookup: {
                from: graphViewName,
                startWith: "root",
                connectFromField: "parent",
                connectToField: "name",
                as: "hierarchy",
                maxDepth: 2,
            },
        },
        {$unionWith: unionViewName},
    ];

    const results = coll.aggregate(pipeline).toArray();

    // 1 doc from main collection with hierarchy + 2 docs from unionWith (type X).
    assert.eq(results.length, 3, results);

    // First doc should have hierarchy from $graphLookup.
    const mainDoc = results[0];
    assert.eq(mainDoc._id, 0, mainDoc);
    assert.gt(mainDoc.hierarchy.length, 0, mainDoc);
    mainDoc.hierarchy.forEach((h) => {
        assert.eq(h.fromGraphView, true, h);
    });

    // Remaining docs should be from $unionWith.
    const unionDocs = results.slice(1);
    unionDocs.forEach((doc) => {
        assert.eq(doc.fromUnionView, true, doc);
        assert.eq(doc.type, "X", doc);
    });

    dropView(unionViewName);
    dropView(graphViewName);
}

// =============================================================================
// View with extension -> $unionWith -> view with different extension
// =============================================================================
{
    // View A: non-desugaring extension.
    const viewAName = makeViewName("view_A");
    assert.commandWorked(testDb.createView(viewAName, collName, [{$testBar: {noop: true}}, {$match: {category: "A"}}]));

    // View B: desugaring extension.
    const viewBName = makeViewName("view_B");
    assert.commandWorked(
        testDb.createView(viewBName, foreignCollName, [{$matchTopN: {filter: {}, sort: {_id: 1}, limit: 2}}]),
    );

    // View C: transform extension that unions with View B.
    const viewCName = makeViewName("view_C");
    assert.commandWorked(
        testDb.createView(viewCName, collName, [
            {$sort: {_id: 1}},
            {$extensionLimit: 2},
            {$addFields: {source: "C"}},
            {$unionWith: {coll: viewBName, pipeline: [{$addFields: {source: "B"}}]}},
        ]),
    );

    // Query View A and union with View C.
    const pipeline = [{$addFields: {source: "A"}}, {$unionWith: viewCName}, {$sort: {_id: 1, source: 1}}];

    const results = testDb[viewAName].aggregate(pipeline).toArray();

    // View A: category "A" docs = 3 (_id 0, 2, 4).
    // View C: first 2 docs from main (_id 0, 1) + first 2 from foreign (_id 10, 20).
    // Total = 3 + 2 + 2 = 7 docs.
    assert.eq(results.length, 7, results);

    const sourceACounts = results.filter((d) => d.source === "A").length;
    const sourceBCounts = results.filter((d) => d.source === "B").length;
    const sourceCCounts = results.filter((d) => d.source === "C").length;

    assert.eq(sourceACounts, 3, "Expected 3 docs from source A");
    assert.eq(sourceBCounts, 2, "Expected 2 docs from source B");
    assert.eq(sourceCCounts, 2, "Expected 2 docs from source C");

    dropView(viewCName);
    dropView(viewBName);
    dropView(viewAName);
}

// =============================================================================
// $graphLookup from view chain with extension at different depths
// =============================================================================
{
    jsTest.log.info("Testing $graphLookup from nested view chain with extensions");

    const chain = createViewChain([
        {suffix: "graphchain_base", sourceColl: graphCollName, pipeline: [{$testBar: {noop: true}}]},
        {suffix: "graphchain_mid", sourceColl: null, pipeline: [{$addFields: {midLevel: true}}]},
        {
            suffix: "graphchain_top",
            sourceColl: null,
            pipeline: [{$testBar: {noop: true}}, {$addFields: {topLevel: true}}],
        },
    ]);

    const pipeline = [
        {$match: {_id: 0}},
        {
            $graphLookup: {
                from: chain.topViewName,
                startWith: "root",
                connectFromField: "parent",
                connectToField: "name",
                as: "chain",
                maxDepth: 3,
            },
        },
    ];

    const results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);

    const chainDocs = results[0].chain;
    assert.gt(chainDocs.length, 0, chainDocs);

    // All chain docs should have fields from all view levels.
    chainDocs.forEach((doc) => {
        assert.eq(doc.midLevel, true, doc);
        assert.eq(doc.topLevel, true, doc);
    });

    chain.cleanup();
}

// =============================================================================
// $unionWith with $graphLookup in subpipeline, both targeting views with extensions
// =============================================================================
{
    jsTest.log.info("Testing $unionWith with $graphLookup in subpipeline targeting extension views");

    // View for the $graphLookup.
    const graphViewName = makeViewName("union_graph_combo");
    assert.commandWorked(
        testDb.createView(graphViewName, graphCollName, [{$testBar: {noop: true}}, {$addFields: {isGraph: true}}]),
    );

    const pipeline = [
        {$match: {_id: 0}},
        {
            $unionWith: {
                coll: collName,
                pipeline: [
                    {$match: {_id: 1}},
                    {
                        $graphLookup: {
                            from: graphViewName,
                            startWith: "child1",
                            connectFromField: "parent",
                            connectToField: "name",
                            as: "ancestors",
                            maxDepth: 2,
                        },
                    },
                ],
            },
        },
    ];

    const results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 2, results);

    // First doc is from main pipeline.
    assert.eq(results[0]._id, 0, results[0]);

    // Second doc is from $unionWith with $graphLookup results.
    assert.eq(results[1]._id, 1, results[1]);
    assert(results[1].hasOwnProperty("ancestors"), results[1]);
    results[1].ancestors.forEach((a) => {
        assert.eq(a.isGraph, true, a);
    });

    dropView(graphViewName);
}

// =============================================================================
// Multiple $unionWith stages in sequence, each targeting views with different extensions
// =============================================================================
{
    // View with non-desugaring extension.
    const viewAName = makeViewName("seq_union_A");
    assert.commandWorked(
        testDb.createView(viewAName, collName, [{$testBar: {noop: true}}, {$match: {x: 1}}, {$addFields: {from: "A"}}]),
    );

    // View with desugaring extension.
    const viewBName = makeViewName("seq_union_B");
    assert.commandWorked(
        testDb.createView(viewBName, collName, [
            {$matchTopN: {filter: {x: {$gte: 4}}, sort: {x: 1}, limit: 2}},
            {$addFields: {from: "B"}},
        ]),
    );

    // View with transform extension.
    const viewCName = makeViewName("seq_union_C");
    assert.commandWorked(
        testDb.createView(viewCName, foreignCollName, [
            {$sort: {_id: 1}},
            {$extensionLimit: 1},
            {$addFields: {from: "C"}},
        ]),
    );

    const pipeline = [
        {$match: {_id: 0}},
        {$addFields: {from: "main"}},
        {$unionWith: viewAName},
        {$unionWith: viewBName},
        {$unionWith: viewCName},
        {$sort: {from: 1, _id: 1}},
    ];

    const results = coll.aggregate(pipeline).toArray();

    // main: 1 doc, A: 1 doc (x=1), B: 2 docs (x>=4), C: 1 doc (first foreign).
    assert.eq(results.length, 5, results);

    const byFrom = {};
    results.forEach((doc) => {
        byFrom[doc.from] = (byFrom[doc.from] || 0) + 1;
    });

    assert.eq(byFrom["main"], 1, byFrom);
    assert.eq(byFrom["A"], 1, byFrom);
    assert.eq(byFrom["B"], 2, byFrom);
    assert.eq(byFrom["C"], 1, byFrom);

    dropView(viewCName);
    dropView(viewBName);
    dropView(viewAName);
}

jsTest.log.info("All nested subpipeline combination tests passed!");
