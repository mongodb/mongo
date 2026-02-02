/**
 * Tests $facet interactions with views containing extensions.
 *
 * Note: Extension stages are generally disallowed directly in $facet subpipelines, but $facet
 * can operate ON views that have extensions in their definition.
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
    {_id: 0, x: 1, category: "A", value: 10},
    {_id: 1, x: 2, category: "B", value: 20},
    {_id: 2, x: 3, category: "A", value: 30},
    {_id: 3, x: 4, category: "B", value: 40},
    {_id: 4, x: 5, category: "A", value: 50},
];
assert.commandWorked(coll.insertMany(documents));

const foreignCollName = collName + "_foreign";
const foreignColl = testDb[foreignCollName];

const foreignDocuments = [
    {_id: 10, data: "alpha", type: "X"},
    {_id: 20, data: "beta", type: "Y"},
    {_id: 30, data: "gamma", type: "X"},
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
// $facet ON views with non-desugaring extension ($testBar)
// =============================================================================
{
    jsTest.log.info("Testing $facet ON view with $testBar (non-desugaring extension)");

    const viewName = makeViewName("testBar_in_def");
    assert.commandWorked(
        testDb.createView(viewName, collName, [{$testBar: {noop: true}}, {$addFields: {fromView: true}}]),
    );

    const pipeline = [
        {
            $facet: {
                categoryA: [{$match: {category: "A"}}, {$sort: {_id: 1}}],
                categoryB: [{$match: {category: "B"}}, {$sort: {_id: 1}}],
                allDocs: [{$sort: {_id: 1}}],
            },
        },
    ];

    const results = testDb[viewName].aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);

    // Verify categoryA results.
    const categoryADocs = results[0].categoryA;
    assert.eq(categoryADocs.length, 3, categoryADocs);
    categoryADocs.forEach((doc) => {
        assert.eq(doc.category, "A", doc);
        assert.eq(doc.fromView, true, doc);
    });

    // Verify categoryB results.
    const categoryBDocs = results[0].categoryB;
    assert.eq(categoryBDocs.length, 2, categoryBDocs);
    categoryBDocs.forEach((doc) => {
        assert.eq(doc.category, "B", doc);
        assert.eq(doc.fromView, true, doc);
    });

    // Verify allDocs results.
    assert.eq(results[0].allDocs.length, documents.length, results[0].allDocs);

    dropView(viewName);
}

// =============================================================================
// $facet ON views with desugaring extension ($matchTopN)
// =============================================================================
{
    jsTest.log.info("Testing $facet ON view with $matchTopN (desugaring extension)");

    const viewName = makeViewName("matchTopN_in_def");
    assert.commandWorked(
        testDb.createView(viewName, collName, [{$matchTopN: {filter: {x: {$gte: 2}}, sort: {x: -1}, limit: 3}}]),
    );

    const pipeline = [
        {
            $facet: {
                topValues: [{$sort: {value: -1}}, {$limit: 2}],
                count: [{$count: "total"}],
            },
        },
    ];

    const results = testDb[viewName].aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);

    // View returns top 3 docs where x >= 2, sorted by x desc: _id=4 (x=5), _id=3 (x=4), _id=2 (x=3).
    // Then facet's topValues sorts by value desc and takes top 2.
    assert.eq(results[0].topValues.length, 2, results[0].topValues);
    assert.eq(results[0].count[0].total, 3, results[0].count);

    dropView(viewName);
}

// =============================================================================
// $facet ON views with desugar+source extension ($readNDocuments)
// =============================================================================
{
    jsTest.log.info("Testing $facet ON view with $readNDocuments (desugar+source extension)");

    const viewName = makeViewName("readNDocuments_in_def");
    assert.commandWorked(testDb.createView(viewName, collName, [{$readNDocuments: {numDocs: 3}}]));

    const pipeline = [
        {
            $facet: {
                filtered: [{$match: {x: {$lte: 2}}}],
                projected: [{$project: {_id: 1, x: 1}}],
            },
        },
    ];

    const results = testDb[viewName].aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);

    // $readNDocuments returns first 3 documents.
    assert.lte(results[0].filtered.length, 3, results[0].filtered);
    assert.eq(results[0].projected.length, 3, results[0].projected);

    dropView(viewName);
}

// =============================================================================
// $facet ON views with transform extension ($extensionLimit)
// =============================================================================
{
    jsTest.log.info("Testing $facet ON view with $extensionLimit (transform extension)");

    const viewName = makeViewName("extensionLimit_in_def");
    assert.commandWorked(testDb.createView(viewName, collName, [{$sort: {_id: 1}}, {$extensionLimit: 4}]));

    const pipeline = [
        {
            $facet: {
                sumValues: [{$group: {_id: null, total: {$sum: "$value"}}}],
                avgX: [{$group: {_id: null, avgX: {$avg: "$x"}}}],
            },
        },
    ];

    const results = testDb[viewName].aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);

    // View returns first 4 documents (sorted by _id): _id 0,1,2,3 with values 10,20,30,40.
    assert.eq(results[0].sumValues[0].total, 100, results[0].sumValues);
    assert.eq(results[0].avgX[0].avgX, 2.5, results[0].avgX);

    dropView(viewName);
}

// =============================================================================
// $facet ON view chain where intermediate view has extension
// =============================================================================
{
    jsTest.log.info("Testing $facet ON view chain where intermediate view has extension");

    // Level 1: View with extension on base collection.
    const level1ViewName = makeViewName("chain_level1");
    assert.commandWorked(
        testDb.createView(level1ViewName, collName, [{$testBar: {noop: true}}, {$addFields: {level1: true}}]),
    );

    // Level 2: View on level 1 with no extension.
    const level2ViewName = makeViewName("chain_level2");
    assert.commandWorked(testDb.createView(level2ViewName, level1ViewName, [{$addFields: {level2: true}}]));

    // Level 3: View on level 2 with extension.
    const level3ViewName = makeViewName("chain_level3");
    assert.commandWorked(
        testDb.createView(level3ViewName, level2ViewName, [{$testBar: {noop: true}}, {$addFields: {level3: true}}]),
    );

    const pipeline = [
        {
            $facet: {
                categoryA: [{$match: {category: "A"}}],
                highValue: [{$match: {value: {$gte: 30}}}],
            },
        },
    ];

    const results = testDb[level3ViewName].aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);

    // Verify all docs have fields from all view levels.
    results[0].categoryA.forEach((doc) => {
        assert.eq(doc.level1, true, doc);
        assert.eq(doc.level2, true, doc);
        assert.eq(doc.level3, true, doc);
    });

    results[0].highValue.forEach((doc) => {
        assert.eq(doc.level1, true, doc);
        assert.eq(doc.level2, true, doc);
        assert.eq(doc.level3, true, doc);
        assert.gte(doc.value, 30, doc);
    });

    dropView(level3ViewName);
    dropView(level2ViewName);
    dropView(level1ViewName);
}

jsTest.log.info("All $facet with views extension tests passed!");
