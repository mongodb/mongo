/**
 * Test that larger queries do not fail. This includes larger aggregation pipelines, as well as
 * large $match/$project stages, deeply nested paths, and many predicates in an $and/$or.
 * The size of these queries was found by trial and error until we reach the BSON size limit.
 *
 * @tags: [
 *   # Can't wrap queries in facets without going past max BSON depth.
 *   do_not_wrap_aggregations_in_facets,
 *   not_allowed_with_signed_security_token,
 *   # Can't use multiplanning, as it leads to query serialization that fails because of max BSON
 *   # size.
 *   does_not_support_multiplanning_single_solutions,
 *   incompatible_aubsan,
 *   requires_profiling
 * ]
 */
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

// Only run this test for debug=off opt=on without sanitizers active. With any of these activated,
// the stack frames are larger and can more easily stack overflow.
if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

// This test can cause lots of spam in the slow query logs due to the size of the queries. If an
// error happens, we'll have a backtrace and know which query is the issue, so slow query logs are
// not necessary.
db.setProfilingLevel(0, {slowms: 10000});

const coll = db.query_limits_test;
coll.drop();

// Multikey so we can't apply any non-multikey optimizations to stress as much as possible.
assert.commandWorked(coll.insert({_id: 0, a: [0, 1], b: [2, 3], c: 4, d: 5, object: {}}));

function range(high) {
    return [...Array(high).keys()];
}

function runAgg(pipeline) {
    // Run pipeline to make sure it doesn't fail.
    const result = coll.aggregate(pipeline).toArray();
}

// Construct a {$match: {a: {$in: [0, 1, 2, ...]}}}.
function testLargeIn() {
    jsTestLog("Testing large $in");
    // Int limit is different than double limit.
    const filterValsInts = range(1200000).map((i) => NumberInt(i));
    runAgg([{$match: {a: {$in: filterValsInts}}}]);

    const filterValsDoubles = range(1000000).map((i) => i * 1.0);
    runAgg([{$match: {a: {$in: filterValsDoubles}}}]);
}

// Construct a big $switch statement.
function testLargeSwitch() {
    jsTestLog("Testing large $switch");
    const cases = range(150000)
        .map(function (i) {
            return {case: {$gt: ["$a", i]}, then: i};
        })
        .reverse();
    runAgg([{$project: {b: {$switch: {branches: cases, default: 345678}}}}]);
}

// Construct a big $bucket statement.
function testLargeBucket() {
    jsTestLog("Testing large $bucket");
    let boundaries = [];
    for (let i = 0; i < 100000; i++) {
        boundaries.push(i);
    }
    runAgg([
        {
            $bucket: {
                groupBy: "$a",
                boundaries: boundaries,
                default: "default",
                output: {"count": {$sum: 1}},
            },
        },
    ]);
}

// Construct a {$project: {a0: 1, a1: 1, ...}}.
function testLargeProject() {
    jsTestLog("Testing large $project");
    const projectFields = {};
    range(1000000).forEach(function (i) {
        projectFields["a" + i] = NumberInt(1);
    });
    runAgg([{$project: projectFields}]);

    const pathSize = 195;
    let nestedProjectField = "a0";
    for (let i = 1; i < pathSize; i++) {
        nestedProjectField += ".a" + i;
    }
    runAgg([{$project: {[nestedProjectField]: 1}}]);
}

// Run $and and $or with many different types of predicates.
function testLargeAndOrPredicates() {
    jsTestLog("Testing large $and/$or predicates");

    // Large $match of the form {$match: {a0: 1, a1: 1, ...}}
    const largeMatch = {};
    range(800000).forEach(function (i) {
        largeMatch["a" + i] = NumberInt(1);
    });
    runAgg([{$match: largeMatch}]);

    function intStream(n) {
        return range(n).map((i) => NumberInt(i));
    }

    const andOrFilters = [
        // Plain a=i filter.
        intStream(500000).map(function (i) {
            return {a: i};
        }),
        // a_i = i filter. Different field for each value.
        intStream(500000).map(function (i) {
            const field = "a" + i;
            return {[field]: i};
        }),
        // Mix of lt and gt with the same field.
        intStream(500000).map(function (i) {
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {a: predicate};
        }),
        // Mix of lt and gt with different fields.
        intStream(400000).map(function (i) {
            const field = "a" + i;
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {[field]: predicate};
        }),
        // Mix of lt and gt wrapped in not with different fields.
        intStream(300000).map(function (i) {
            const field = "a" + i;
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {[field]: {$not: predicate}};
        }),
        // $exists on different fields.
        intStream(400000).map(function (i) {
            const field = "a" + i;
            return {[field]: {$exists: true}};
        }),
        intStream(400000).map(function (i) {
            const field = "a" + i;
            return {[field]: {$exists: false}};
        }),
    ];
    for (const m of andOrFilters) {
        runAgg([{$match: {$and: m}}]);
        runAgg([{$match: {$or: m}}]);
    }
}

function testLongFieldNames() {
    jsTestLog("Testing $match with long field name");
    // Test with a long field name that's accepted by the server.
    {
        const longFieldName = "a".repeat(10_000_000);
        const predicate = {[longFieldName]: 1};
        runAgg([{$match: predicate}]);
        runAgg([{$match: {$and: [predicate]}}]);
        runAgg([{$match: {$or: [predicate]}}]);
    }

    // Test with a field name that's too long, where the server rejects it.
    {
        const extraLongFieldName = "a".repeat(17_000_000);
        const predicate = {[extraLongFieldName]: 1};
        assert.throwsWithCode(() => runAgg([{$match: predicate}]), 17260);
        assert.throwsWithCode(() => runAgg([{$match: {$and: [predicate]}}]), 17260);
        assert.throwsWithCode(() => runAgg([{$match: {$or: [predicate]}}]), 17260);
    }
}

// Test deeply nested queries.
function testDeeplyNestedPath() {
    jsTestLog("Testing deeply nested $match");
    let deepQuery = {a: {$eq: 1}};
    const depth = 72;
    for (let i = 0; i < depth; i++) {
        deepQuery = {a: {$elemMatch: deepQuery}};
    }
    runAgg([{$match: deepQuery}]);
}

// Test pipeline length.
function testPipelineLimits() {
    jsTestLog("Testing large agg pipelines");
    const pipelineLimit = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalPipelineLengthLimit: 1}),
    ).internalPipelineLengthLimit;
    let stages = [
        {$limit: 1},
        {$skip: 1},
        {$sort: {a: 1}},
        {$unwind: "$a"},
        {$match: {a: {$mod: [4, 2]}}},
        {$group: {_id: "$a"}},
        {$addFields: {c: {$add: ["$c", "$d"]}}},
        {$addFields: {a: 5}},
        {$project: {a: 1}},
        {$match: {a: 1}},
    ];

    for (const stage of stages) {
        const pipeline = range(pipelineLimit).map((_) => stage);
        jsTestLog(stage);
        runAgg(pipeline);
    }
}

/*
 * Generates a $match query with specified branchingFactor and maxDepth of the form
 * {$and: [{$or: [... $and ...]}, ... (length branchingFactor) ...]}
 * Uses unique field names across the generated query.
 */
let fieldIndex = 0;
function generateNestedAndOrHelper(type, branchingFactor, maxDepth) {
    if (maxDepth === 0) {
        const field = "a" + fieldIndex;
        const query = {[field]: NumberInt(fieldIndex)};
        fieldIndex++;
        return query;
    }

    const oppositeType = type === "$and" ? "$or" : "$and";
    const children = [];
    for (let i = 0; i < branchingFactor; i++) {
        const childQuery = generateNestedAndOrHelper(oppositeType, branchingFactor, maxDepth - 1);
        children.push(childQuery);
    }

    return {[type]: children};
}

function generateNestedAndOr(type, branchingFactor, maxDepth) {
    fieldIndex = 0;
    return generateNestedAndOrHelper(type, branchingFactor, maxDepth);
}

function testNestedAndOr() {
    jsTestLog("Testing nested $and/$or");
    for (const topLevelType of ["$and", "$or"]) {
        // Test different types of nested queries
        let [branchingFactor, maxDepth] = [3, 10];
        const deepNarrowQuery = generateNestedAndOr(topLevelType, branchingFactor, maxDepth);
        runAgg([{$match: deepNarrowQuery}]);

        [branchingFactor, maxDepth] = [10, 5];
        const shallowWideQuery = generateNestedAndOr(topLevelType, branchingFactor, maxDepth);
        runAgg([{$match: shallowWideQuery}]);
    }
}

function testLargeSetFunction() {
    jsTestLog("Testing large $setIntersection");

    const fieldExprs = [];
    for (let j = 1; j <= 750000; j++) {
        fieldExprs.push("$a" + j);
    }
    const pipeline = [{$project: {a: {$setIntersection: fieldExprs}}}, {$group: {_id: "$a"}}];
    runAgg(pipeline);
}

function testLargeConcatFunction() {
    jsTestLog("Testing large $concat");

    const fieldExprs = [];
    for (let j = 1; j <= 750000; j++) {
        fieldExprs.push("$a" + j);
    }
    const pipeline = [{$project: {a: {$concat: fieldExprs}}}];
    runAgg(pipeline);
}

function testLargeArrayToObjectFunction() {
    jsTestLog("Testing large $arrayToObject");

    const fieldExprs = [];
    for (let j = 1; j <= 200000; j++) {
        fieldExprs.push(["a" + j, j]);
    }
    const pipeline = [{$project: {a: {$arrayToObject: [fieldExprs]}}}];
    runAgg(pipeline);
}

const tests = [
    testLargeIn,
    testLargeSwitch,
    testLargeBucket,
    testLargeProject,
    testLargeAndOrPredicates,
    testLongFieldNames,
    testDeeplyNestedPath,
    testNestedAndOr,
    testPipelineLimits,
    testLargeSetFunction,
    testLargeConcatFunction,
    testLargeArrayToObjectFunction,
];

for (const test of tests) {
    test();
}
