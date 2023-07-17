/**
 * Test that larger queries do not fail. This includes larger aggregation pipelines, as well as
 * large $match/$project stages, deeply nested paths, and many predicates in an $and/$or.
 * The size of these queries was found by trial and error until we reach the BSON size limit.
 *
 * @tags: [
 *   # Can't wrap queries in facets without going past max BSON depth.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

import {checkCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

const debugBuild = db.adminCommand("buildInfo").debug;
// For debug builds we create smaller queries so the test runs in a reasonable amount of time.
const debugDivider = debugBuild ? 100 : 1;
const isBonsaiEnabled = checkCascadesOptimizerEnabled(db);
const isSBEEnabled = checkSBEEnabled(db);

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
    // TODO: SERVER-78631 remove check after ticket is done. These are Bonsai-specific issues with
    // PSR.
    if (!isBonsaiEnabled) {
        // Int limit is different than double limit.
        const filterValsInts = range(1200000 / debugDivider).map(i => NumberInt(i));
        runAgg([{$match: {a: {$in: filterValsInts}}}]);

        const filterValsDoubles = range(1000000 / debugDivider).map(i => i * 1.0);
        runAgg([{$match: {a: {$in: filterValsDoubles}}}]);
    }
}

// Construct a {$project: {a0: 1, a1: 1, ...}}.
function testLargeProject() {
    // TODO: SERVER-78580 uncomment this test. This is a project parsing issue affecting all
    // engines.
    // const projectFields = {};
    // range(1000000 / debugDivider).forEach(function(i) {
    //     projectFields["a" + i] = NumberInt(1);
    // });
    // runAgg([{$project: projectFields}]);

    const pathSize = 10000000 / debugDivider;
    let nestedProjectField = "a0";
    for (let i = 1; i < pathSize; i++) {
        nestedProjectField += ".a" + i;
    }
    runAgg([{$project: {nestedProjectField: 1}}]);
}

// Run $and and $or with many different types of predicates.
function testLargeAndOrPredicates() {
    // TODO: SERVER-78635
    // TODO: SERVER-78631 remove this early return once this and the ticket above are done. These
    // are Bonsai-specific issues with PSR.
    if (isBonsaiEnabled) {
        return;
    }
    // TODO: SERVER-78587 remove the SBE check. This is an issue with compiling expressions to the
    // SBE VM, so it affects stage builders and Bonsai.
    if (isSBEEnabled) {
        return;
    }

    // Large $match of the form {$match: {a0: 1, a1: 1, ...}}
    const largeMatch = {};
    range(1200000 / debugDivider).forEach(function(i) {
        largeMatch["a" + i] = NumberInt(1);
    });
    runAgg([{$match: largeMatch}]);

    function intStream(n) {
        return range(n / debugDivider).map(i => NumberInt(i));
    }

    const andOrFilters = [
        // Plain a=i filter.
        intStream(800000).map(function(i) {
            return {a: i};
        }),
        // a_i = i filter. Different field for each value.
        intStream(600000).map(function(i) {
            const field = "a" + i;
            return {[field]: i};
        }),
        // Mix of lt and gt with the same field.
        intStream(500000).map(function(i) {
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {a: predicate};
        }),
        // Mix of lt and gt with different fields.
        intStream(400000).map(function(i) {
            const field = "a" + i;
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {[field]: predicate};
        }),
        // Mix of lt and gt wrapped in not with different fields.
        intStream(300000).map(function(i) {
            const field = "a" + i;
            const predicate = i % 2 ? {$lt: i} : {$gt: i};
            return {[field]: {$not: predicate}};
        }),
        // $exists on different fields.
        intStream(400000).map(function(i) {
            const field = "a" + i;
            return {[field]: {$exists: true}};
        }),
        intStream(400000).map(function(i) {
            const field = "a" + i;
            return {[field]: {$exists: false}};
        })
    ];
    for (const m of andOrFilters) {
        runAgg([{$match: {$and: m}}]);
        runAgg([{$match: {$or: m}}]);
    }
}

// Test deeply nested queries.
function testDeeplyNestedPath() {
    let deepQuery = {a: {$eq: 1}};
    const depth = debugBuild ? 40 : 72;
    for (let i = 0; i < depth; i++) {
        deepQuery = {a: {$elemMatch: deepQuery}};
    }
    runAgg([{$match: deepQuery}]);
}

// Test pipeline length.
function testPipelineLimits() {
    const pipelineLimit = debugBuild ? 200 : 1000;
    let stages = [
        {$limit: 1},
        {$skip: 1},
        {$sort: {a: 1}},
        {$unwind: "$a"},
        {$addFields: {c: {$add: ["$c", "$d"]}}},
        {$addFields: {a: 5}},
        {$match: {a: {$mod: [4, 2]}}},
    ];

    if (!isBonsaiEnabled) {
        // TODO: SERVER-78354 should move $project, $addFields, and $unwind to "stages" so $project
        // runs with Bonsai. This is an issue with the reference tracker.
        stages.push({$project: {a: 1}});
        stages.push({$match: {a: 1}});
    }
    if (!isSBEEnabled && !isBonsaiEnabled) {
        // TODO: SERVER-78477 should move this to "stages" so $group runs with SBE. This is an issue
        // with SBE stagebuilders.
        stages.push({$group: {_id: "$a"}});
    }
    for (const stage of stages) {
        const pipeline = range(pipelineLimit).map(_ => stage);
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
        const field = 'a' + fieldIndex;
        const query = {[field]: NumberInt(fieldIndex)};
        fieldIndex++;
        return query;
    }

    const oppositeType = type === '$and' ? '$or' : '$and';
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
    for (const topLevelType of ['$and', '$or']) {
        // Test different types of nested queries
        let [branchingFactor, maxDepth] = debugBuild ? [3, 6] : [3, 10];
        const deepNarrowQuery = generateNestedAndOr(topLevelType, branchingFactor, maxDepth);
        runAgg([{$match: deepNarrowQuery}]);

        [branchingFactor, maxDepth] = debugBuild ? [6, 3] : [10, 5];
        const shallowWideQuery = generateNestedAndOr(topLevelType, branchingFactor, maxDepth);
        runAgg([{$match: shallowWideQuery}]);
    }
}

const tests = [
    testLargeIn,
    testLargeProject,
    testLargeAndOrPredicates,
    testDeeplyNestedPath,
    testNestedAndOr,
    testPipelineLimits
];

for (const test of tests) {
    test();
}
