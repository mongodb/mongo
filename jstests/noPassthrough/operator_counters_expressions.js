/**
 * Tests aggregate expression counters.
 * @tags: [
 * ]
 */

(function() {
"use strict";
load("jstests/libs/doc_validation_utils.js");  // for assertDocumentValidationFailure

const mongod = MongoRunner.runMongod();
const db = mongod.getDB(jsTest.name());
const collName = jsTest.name();
const coll = db[collName];
coll.drop();

for (let i = 0; i < 3; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        x: i,
        "a$b": "foo",
        "a.b": "bar",
    }));
}

/**
 * Execute the `command` and compare the operator counters with the expected values.
 * @param {*} command to be executed.
 * @param {*} expectedCounters  expected operator counters.
 */
function checkCounters(command, expectedCounters) {
    const origCounters = db.serverStatus().metrics.operatorCounters.expressions;

    command();

    const newCounters = db.serverStatus().metrics.operatorCounters.expressions;
    let actualCounters = {};
    for (let ec in origCounters) {
        const diff = newCounters[ec] - origCounters[ec];
        if (diff !== 0) {
            actualCounters[ec] = diff;
        }
    }

    assert.docEq(expectedCounters, actualCounters);
}

/**
 * Check operator counters in the `find`.
 * @param {*} query to be executed.
 * @param {*} expectedCounters - expected operator counters.
 * @param {*} expectedCount - expected number of records returned by the `find` function.
 */
function checkFindCounters(query, expectedCounters, expectedCount) {
    checkCounters(() => assert.eq(expectedCount, coll.find(query).itcount()), expectedCounters);
}

/**
 * Check operator counters in the `aggregate`.
 * @param {*} pipeline to be executed.
 * @param {*} expectedCounters - expected operator counters.
 * @param {*} expectedCount - expected number of records returned by the `find` function.
 */
function checkAggregationCounters(pipeline, expectedCounters, expectedCount) {
    checkCounters(() => assert.eq(expectedCount, coll.aggregate(pipeline).itcount()),
                  expectedCounters);
}

// Find.
checkFindCounters(
    {$expr: {$eq: [{$getField: "a$b"}, "foo"]}}, {"$getField": 2, "$eq": 2, "$const": 2}, 3);

// Update.
checkCounters(() => assert.commandWorked(coll.update(
                  {_id: 0, $expr: {$eq: [{$getField: {field: "a$b", input: "$$ROOT"}}, "foo"]}},
                  {$set: {y: 10}})),
              {"$getField": 4, "$const": 6, "$eq": 4});

checkCounters(
    () => assert.commandWorked(coll.update(
        {_id: 1}, [{$replaceWith: {$setField: {field: "a.b", input: "$$ROOT", value: "qqq"}}}])),
    {"$setField": 1});

checkCounters(() => assert.commandWorked(db.runCommand({
    update: coll.getName(),
    updates: [{
        q: {_id: 1},
        u: [{$replaceWith: {$setField: {field: "a.b", input: "$$ROOT", value: "zzz"}}}],
        upsert: false
    }]
})),
              {"$setField": 1});

// Delete.
checkCounters(() => assert.commandWorked(db.runCommand({
    delete: coll.getName(),
    deletes: [{q: {"_id": {$gt: 1}, $expr: {$eq: [{$getField: "a$b"}, "foo"]}}, limit: 1}]
})),
              {"$getField": 4, "$eq": 4, "$const": 6});

// In aggregation pipeline.
let pipeline = [{$project: {_id: 1, test: {$getField: "a$b"}}}];
checkAggregationCounters(pipeline, {"$getField": 5, "$const": 4}, 2);

pipeline = [{$match: {_id: 1, $expr: {$eq: [{$getField: "a$b"}, "foo"]}}}];
checkAggregationCounters(pipeline, {"$getField": 5, "$eq": 5, "$const": 6}, 1);

pipeline =
    [{$match: {_id: 1, $expr: {$eq: [{$getField: {field: "a$b", input: {"a$b": "b"}}}, "b"]}}}];
checkAggregationCounters(pipeline, {"$getField": 5, "$const": 9, "$eq": 5}, 1);

pipeline = [{
    $project: {_id: 1, test: {$setField: {field: {$const: "a.b"}, input: "$$ROOT", value: "barrr"}}}
}];
checkAggregationCounters(pipeline, {"$setField": 5, "$const": 9}, 2);

// With sub-pipeline.
const testColl = db.operator_counters_expressions2;

function initTestColl(i) {
    testColl.drop();
    assert.commandWorked(testColl.insert({
        _id: i,
        x: i,
        "a$b": "bar",
    }));
}

initTestColl(5);
pipeline = [
    {$project: {x: 1, _id: 0, test: {$getField: "a$b"}}},
    {
        $unionWith: {
            coll: testColl.getName(),
            pipeline: [{$project: {x: 1, _id: 0, test: {$getField: "a$b"}}}]
        }
    }
];
checkAggregationCounters(pipeline, {"$getField": 10, "$const": 8}, 3);

initTestColl(5);
pipeline = [
    {$lookup: {
        from: testColl.getName(),
        pipeline: [{$project: {x: 1, _id: 0, test: {$getField: "a$b"}}}],
        as: "joinedField"
}}];
checkAggregationCounters(pipeline, {"$getField": 9, "$const": 6}, 2);

initTestColl(1);
let mergePipeline =
    [{$project: {test: {$setField: {field: "a$b", input: "$$ROOT", value: "merged"}}}}];
pipeline = [{
    $merge:
        {into: testColl.getName(), on: "_id", whenMatched: mergePipeline, whenNotMatched: "insert"}
}];
checkCounters(() => {
    coll.aggregate(pipeline).itcount();
    assert.eq(2, testColl.find().itcount());
}, {"$setField": 4, "$const": 4});

// Expressions in view pipeline.
db.view.drop();
let viewPipeline = [{$match: {$expr: {$eq: [{$getField: "a.b"}, "bar"]}}}];
assert.commandWorked(
    db.runCommand({create: "view", viewOn: coll.getName(), pipeline: viewPipeline}));
checkCounters(() => db.view.find().itcount(), {"$getField": 3, "$const": 2, "$eq": 3});

// Expressions in document validator.
checkCounters(() => {
    initTestColl(1);
    assert.commandWorked(db.runCommand(
        {"collMod": testColl.getName(), "validator": {$expr: {$eq: [{$getField: "a$b"}, "new"]}}}));
}, {"$getField": 1, "$eq": 1});

// Expression counter is not incremented for each validated document.
checkCounters(() => {
    assert.commandWorked(testColl.insert({
        _id: 2,
        x: 2,
        "a$b": "new",
    }));
    assertDocumentValidationFailure(testColl.insert({
        _id: 3,
        x: 3,
        "a$b": "invalid",
    }),
                                    testColl);
}, {});

// $cond
pipeline = [{$project: {item: 1, discount: {$cond: {if: {$gte: ["$x", 1]}, then: 10, else: 0}}}}];
checkAggregationCounters(pipeline, {"$cond": 5, "$gte": 5, "$const": 12}, 2);

// $ifNull
pipeline = [{$project: {description: {$ifNull: ["$description", "Unspecified"]}}}];
checkAggregationCounters(pipeline, {"$ifNull": 5, "$const": 4}, 2);

// $divide, $switch
let query = {
    $expr: {
        $eq: [
            {
                $switch: {
                    branches: [{case: {$gt: ["$x", 0]}, then: {$divide: ["$x", 2]}}],
                    default: {$subtract: [100, "$x"]}
                }
            },
            100
        ]
    }
};
checkFindCounters(
    query, {"$const": 4, "$divide": 2, "$subtract": 2, "$eq": 2, "$gt": 2, "$switch": 2}, 1);

// $cmp, $exp, $abs, $range
pipeline = [{
    $project: {
        cmpField: {$cmp: ["$x", 250]},
        expField: {$exp: "$x"},
        absField: {$abs: "$x"},
        rangeField: {$range: [0, "$x", 25]}
    }
}];
checkAggregationCounters(pipeline, {"$abs": 5, "$cmp": 5, "$const": 12, "$exp": 5, "$range": 5}, 2);

// $or
pipeline = [{$match: {$expr: {$or: [{$eq: ["$_id", 0]}, {$eq: ["$x", 1]}]}}}];
checkAggregationCounters(pipeline, {"$const": 2, "$eq": 6, "$or": 3}, 2);

// $dateFromParts
pipeline =
    [{$project: {date: {$dateFromParts: {'year': 2021, 'month': 10, 'day': {$add: ['$x', 10]}}}}}];
checkAggregationCounters(pipeline, {"$add": 5, "$const": 12, "$dateFromParts": 5}, 2);

// $concat
pipeline = [{$project: {mystring: {$concat: [{$getField: "a$b"}, {$getField: "a.b"}]}}}];
checkAggregationCounters(pipeline, {"$concat": 5, "$const": 8, "$getField": 10}, 2);

// $toDouble
pipeline = [{$project: {doubleval: {$toDouble: "$_id"}}}];
checkAggregationCounters(pipeline, {"$const": 4, "$convert": 4, "$toDouble": 1}, 2);

// $setIntersection
pipeline = [{$project: {intersection: {$setIntersection: [[1, 2, 3], [3, 2]]}}}];
checkAggregationCounters(pipeline, {"$const": 8, "$setIntersection": 2}, 2);

MongoRunner.stopMongod(mongod);
})();
