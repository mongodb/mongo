/**
 * Tests that boolean expression simplifier produces expected metrics.
 */
function assertExpressionSimplificationMetrics(initialMetrics, newMetrics, expectedMetrics) {
    assert.eq(newMetrics.trivial, initialMetrics.trivial + expectedMetrics.trivial);
    assert.eq(newMetrics.abortedTooLarge,
              initialMetrics.abortedTooLarge + expectedMetrics.abortedTooLarge);
    assert.eq(newMetrics.notSimplified,
              initialMetrics.notSimplified + expectedMetrics.notSimplified);
    assert.eq(newMetrics.simplified, initialMetrics.simplified + expectedMetrics.simplified);
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryEnableBooleanExpressionsSimplifier: true,
        internalQueryMaximumNumberOfUniquePredicatesToSimplify: 10
    }
});
const db = conn.getDB(jsTestName());
const coll = db.getCollection(jsTestName());
coll.drop();

assert.commandWorked(db.coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 1},
    {a: 2, b: 2},
]));

// save initial metrics
let exprSimplificationMetrics;
const initExprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;

// Run with trivial query - 1
{
    const filter = {b: 2};
    assert.commandWorked(coll.find(filter).explain());
    exprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;
    assertExpressionSimplificationMetrics(
        initExprSimplificationMetrics,
        exprSimplificationMetrics,
        {trivial: 1, abortedTooLarge: 0, notSimplified: 0, simplified: 0});
}

// Run with trivial query - 2
{
    const filter = {$and: [{b: 2}]};
    assert.commandWorked(coll.find(filter).explain());
    exprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;
    assertExpressionSimplificationMetrics(
        initExprSimplificationMetrics,
        exprSimplificationMetrics,
        {trivial: 2, abortedTooLarge: 0, notSimplified: 0, simplified: 0});
}

// Run with query that aborted because it became too large
{
    const filter = {
        $and: [
            {a: 1},
            {b: 2},
            {c: 3},
            {d: 4},
            {e: 5},
            {f: 6},
            {g: 7},
            {h: 8},
            {i: 9},
            {j: 10},
            {k: 11}
        ]
    };
    assert.commandWorked(coll.find(filter).explain());
    exprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;
    assertExpressionSimplificationMetrics(
        initExprSimplificationMetrics,
        exprSimplificationMetrics,
        {trivial: 2, abortedTooLarge: 1, notSimplified: 0, simplified: 0});
}

// Run with query that completes without simplification - 1
{
    const filter = {
        $and: [
            {$or: [{a: {$eq: 1}}, {b: {$eq: 1}}]},
            {$or: [{c: {$eq: 2}}, {d: {$eq: 2}}]},
        ]
    };
    assert.commandWorked(coll.find(filter).explain());
    exprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;
    assertExpressionSimplificationMetrics(
        initExprSimplificationMetrics,
        exprSimplificationMetrics,
        {trivial: 2, abortedTooLarge: 1, notSimplified: 1, simplified: 0});
}

// Run with query that completes without simplification - 2
{
    const filter = {$and: [{a: 2}, {b: 2}]};
    assert.commandWorked(coll.find(filter).explain());
    exprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;
    jsTest.log(exprSimplificationMetrics);
    assertExpressionSimplificationMetrics(
        initExprSimplificationMetrics,
        exprSimplificationMetrics,
        {trivial: 2, abortedTooLarge: 1, notSimplified: 2, simplified: 0});
}

// Run with query that completes with simplification - 1
{
    const filter = {'$or': [{'$and': [{a: 1}, {a: {'$ne': 1}}]}, {b: 2}]};
    assert.commandWorked(coll.find(filter).explain());
    exprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;
    assertExpressionSimplificationMetrics(
        initExprSimplificationMetrics,
        exprSimplificationMetrics,
        {trivial: 2, abortedTooLarge: 1, notSimplified: 2, simplified: 1});
}

// Run with query that completes with simplification - 2
{
    const filter = {
        '$and': [
            {b: 0},
            {
                '$or': [
                    {a: 0},
                    {
                        '$and': [
                            {c: {'$in': [34, 45]}},
                            {c: {'$nin': [34, 45]}},
                        ]
                    }
                ]
            },
        ]
    };
    assert.commandWorked(coll.find(filter).explain());
    exprSimplificationMetrics = db.serverStatus().metrics.query.expressionSimplifier;
    assertExpressionSimplificationMetrics(
        initExprSimplificationMetrics,
        exprSimplificationMetrics,
        {trivial: 2, abortedTooLarge: 1, notSimplified: 2, simplified: 2});
}

MongoRunner.stopMongod(conn);
