/**
 * Tests edge cases and error handling for subpipelines with views and extensions.
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
    {_id: 0, x: 1, value: 10},
    {_id: 1, x: 2, value: 20},
    {_id: 2, x: 3, value: 30},
    {_id: 3, x: 4, value: 40},
    {_id: 4, x: 5, value: 50},
];
assert.commandWorked(coll.insertMany(documents));

const foreignCollName = collName + "_foreign";
const foreignColl = testDb[foreignCollName];

const foreignDocuments = [
    {_id: 10, data: "alpha"},
    {_id: 20, data: "beta"},
    {_id: 30, data: "gamma"},
];
assert.commandWorked(foreignColl.insertMany(foreignDocuments));

// Helper to create unique view names.
let viewCounter = 0;
function makeViewName(suffix) {
    return jsTestName() + "_view_" + viewCounter++ + "_" + suffix;
}

function dropView(viewName) {
    testDb[viewName].drop();
}

// =============================================================================
// Empty view scenarios
// =============================================================================

// Test $graphLookup from view with extension when no graph connections exist.
{
    const viewName = makeViewName("empty_graph_view");
    assert.commandWorked(testDb.createView(viewName, collName, [{$testBar: {noop: true}}, {$match: {x: {$gt: 1000}}}]));

    const pipeline = [
        {$sort: {_id: 1}},
        {$limit: 1},
        {
            $graphLookup: {
                from: viewName,
                startWith: "$x",
                connectFromField: "x",
                connectToField: "value",
                as: "graph_results",
            },
        },
    ];

    const results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    assert.eq(results[0].graph_results.length, 0, results);

    dropView(viewName);
}

// Test $unionWith with empty view with extension.
{
    const viewName = makeViewName("empty_union_view");
    assert.commandWorked(
        testDb.createView(viewName, foreignCollName, [{$testBar: {noop: true}}, {$match: {data: "NONEXISTENT"}}]),
    );

    const pipeline = [{$sort: {_id: 1}}, {$unionWith: viewName}];

    const results = coll.aggregate(pipeline).toArray();
    // Only main collection documents, none from empty view.
    assert.eq(results.length, documents.length, results);

    dropView(viewName);
}

// Test $facet with empty results from view with extension.
{
    const viewName = makeViewName("empty_facet_view");
    assert.commandWorked(testDb.createView(viewName, collName, [{$testBar: {noop: true}}, {$match: {x: {$gt: 1000}}}]));

    const pipeline = [
        {
            $facet: {
                nonEmpty: [{$match: {x: {$gte: 1}}}],
                empty: [{$match: {x: {$gt: 1000}}}],
            },
        },
    ];

    const results = testDb[viewName].aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    assert.eq(results[0].nonEmpty.length, 0, results);
    assert.eq(results[0].empty.length, 0, results);

    dropView(viewName);
}

// =============================================================================
// Large result set scenarios
// =============================================================================

// Test nested $unionWith with extensions handling moderate result sets.
{
    const viewName = makeViewName("moderate_results");
    assert.commandWorked(testDb.createView(viewName, collName, [{$testBar: {noop: true}}]));

    const pipeline = [{$unionWith: viewName}, {$unionWith: viewName}, {$sort: {_id: 1}}];

    const results = coll.aggregate(pipeline).toArray();
    // Original + 2 copies from unionWith.
    assert.eq(results.length, documents.length * 3, results);

    dropView(viewName);
}

// =============================================================================
// Deep nesting $unionWith
// =============================================================================
{
    const viewName = makeViewName("deep_nest");
    assert.commandWorked(testDb.createView(viewName, foreignCollName, [{$testBar: {noop: true}}, {$limit: 1}]));

    // 4 levels of nesting.
    const pipeline = [
        {$sort: {_id: 1}},
        {$limit: 1},
        {
            $unionWith: {
                coll: collName,
                pipeline: [
                    {$sort: {_id: 1}},
                    {$limit: 1},
                    {
                        $unionWith: {
                            coll: collName,
                            pipeline: [{$sort: {_id: 1}}, {$limit: 1}, {$unionWith: viewName}],
                        },
                    },
                ],
            },
        },
    ];

    const results = coll.aggregate(pipeline).toArray();
    // 1 + 1 + 1 + 1 (from view) = 4 documents.
    assert.eq(results.length, 4, results);

    dropView(viewName);
}

jsTest.log.info("All subpipeline views edge case tests passed!");
