/**
 * Tests that $facet applies $unionWith views correctly
 */

const conn = MongoRunner.runMongod({
    setParameter: {featureFlagExtensionsInsideHybridSearch: true, featureFlagExtensionsAPI: true},
});
const db = conn.getDB(jsTestName());

const coll = db.coll;
const foreignColl = db.foreign;

assert.commandWorked(
    coll.insertMany([
        {_id: 0, x: 1},
        {_id: 1, x: 2},
    ]),
);
assert.commandWorked(
    foreignColl.insertMany([
        {_id: 0, x: 10},
        {_id: 1, x: 20},
        {_id: 2, x: 30},
    ]),
);

// Create a view that adds a 'fromView' marker so we can verify the view's pipeline was applied.
assert.commandWorked(
    db.createView("foreignView", foreignColl.getName(), [{$addFields: {fromView: true}}]),
);

const result = coll
    .aggregate([
        {
            $facet: {
                combined: [
                    {$unionWith: {coll: "foreignView", pipeline: [{$match: {x: {$gte: 10}}}]}},
                ],
            },
        },
    ])
    .toArray();

assert.eq(result.length, 1, "Expected one $facet result document", {result});
const combined = result[0].combined;

// All three foreign docs match {x >= 10} and the view's $addFields must have been applied.
const fromViewDocs = combined.filter((doc) => doc.fromView === true);
assert.eq(fromViewDocs.length, 3, "Expected 3 docs with fromView:true; view stages were dropped", {
    combined,
});

MongoRunner.stopMongod(conn);
