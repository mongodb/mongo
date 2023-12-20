/**
 * Test the ability of the optimizer to push top-level fields from FilterNode and
 * EvaluationNode expressions into a physical scan. The result of such pushdown is
 * that top-level PathGet operations are removed, and instead the physical scan
 * directly produces a projection with the field that was retrieved by PathGet.
 */
import {
    checkCascadesOptimizerEnabled,
    leftmostLeafStage,
    navigateToPlanPath,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const t = db.cqf_field_pushdown;
t.drop();

for (let i = 0; i < 33; i++) {
    assert.commandWorked(t.insert({a1: i, b1: i, c1: i}));
}

class TestQuery {
    constructor(qid, q, qt, spj, skipSarg) {
        // An ID of the query to print in the case of failure
        this.queryId = qid;
        // A find or match query
        this.query = q;
        // The type of the query - 'agg' or 'find'
        this.queryType = qt;
        // The projections that we're testing for
        this.scanProjections = spj;
        // If true, test both with and without SargableNode
        this.skipSargable = skipSarg;
    }
}

function addTest(query, queryType, scanProjections, skipSargable) {
    const q = new TestQuery(tests.length, query, queryType, scanProjections, skipSargable);
    tests.push(q);
}

function getScanProjections(query, coll, aggOrFind) {
    var explain = null;
    switch (aggOrFind) {
        case 'agg':
            explain = coll.explain().aggregate(query);
            break;
        case 'find':
            explain = coll.find(query).explain();
            break;
        default:
            throw new Error('Unknown query type.');
    }

    const scanNode = leftmostLeafStage(explain);
    assert.eq(scanNode.nodeType, "PhysicalScan");
    return scanNode.fieldProjectionMap;
}

function runTest(testQuery, coll) {
    print(`Testing queryId ${testQuery.queryId}.`);

    const scanProjections1 = runWithParams(
        [{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false}],
        () => getScanProjections(testQuery.query, coll, testQuery.queryType));
    assert.eq(Object.keys(scanProjections1), testQuery.scanProjections);

    if (testQuery.skipSarg) {
        const scanProjections2 = runWithParams(
            [{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false}],
            () => getScanProjections(testQuery.query, coll, testQuery.queryType));
        assert.eq(Object.keys(scanProjections2), testQuery.scanProjections);
    }
}

// An array of test queries to run
var tests = [];

// Predicates in match expression format
const eq1 = {
    $eq: ['$f1', 3]
};
const eq2 = {
    $eq: ['$f2', 4]
};
const eq3 = {
    $eq: ['$f3', 5]
};
const or_expr_1 = {
    $or: [{$eq: ["$f1", 7]}, {$eq: ["$f1", 9]}]
};
const or_expr_2 = {
    $or: [{$eq: ["$f2", 70]}, {$eq: ["$f2", 90]}]
};
const and_expr_12 = {
    $and: [or_expr_1, or_expr_2, eq1, eq2]
};

// Predicate field pushdown tests
addTest([{$match: {$expr: {$and: [eq1, eq2, eq3]}}}], "agg", ["<root>", "f1", "f2", "f3"], true);
addTest([{$match: {$expr: and_expr_12}}], "agg", ["<root>", "f1", "f2"], true);
addTest([{$match: {$expr: {$or: [and_expr_12, eq1, eq2]}}}], "agg", ["<root>", "f1", "f2"], true);

addTest({'a.b': "xyz", 'a.a.c': "my", 'b.c': "abc", 'b': 13}, "find", ["<root>", "a", "b"], true);
addTest({'f1': {$elemMatch: {$gt: 1, $lt: 3}}}, "find", ["<root>", "f1"], true);
addTest({'f1': {$elemMatch: {$elemMatch: {$eq: 3}}}}, "find", ["<root>", "f1"], true);

// TODO: SERVER-83937 enable the below tests to work without SargableNode

// Projection field pushdown tests
addTest(
    [
        {$addFields: {x: '$a1', y: '$b1', z: '$c1'}},
        {$addFields: {a: {$add: ["$x", "$y"]}, b: {$add: ["$y", "$z"]}}},
        {$project: {_id: 0, out: "$b"}}
    ],
    "agg",
    ["b1", "c1"],
    false);

// Mixed predicate and projection field pushdown tests
addTest([{$match: {$expr: {$and: [eq1, eq2, eq3]}}}, {$project: {_id: 0, out: '$f3'}}],
        "agg",
        ["<root>", "f3"],
        false);

addTest([{$match: {$expr: {$and: [or_expr_1, or_expr_2]}}}, {$project: {_id: 0, 'f2': 1}}],
        "agg",
        ["<root>", "f2"],
        false);

// Run all tests
for (const tst of tests) {
    runTest(tst, t);
}
