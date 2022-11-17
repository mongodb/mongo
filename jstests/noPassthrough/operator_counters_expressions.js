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
checkFindCounters({$expr: {$eq: [{$getField: "a$b"}, "foo"]}}, {"$getField": 1, "$eq": 1}, 3);

// Update.
checkCounters(() => assert.commandWorked(coll.update(
                  {_id: 0, $expr: {$eq: [{$getField: {field: "a$b", input: "$$ROOT"}}, "foo"]}},
                  {$set: {y: 10}})),
              {"$getField": 1, "$eq": 1});

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
              {"$getField": 1, "$eq": 1});

// In aggregation pipeline.
let pipeline = [{$project: {_id: 1, test: {$getField: "a$b"}}}];
checkAggregationCounters(pipeline, {"$getField": 1}, 2);

pipeline = [{$match: {_id: 1, $expr: {$eq: [{$getField: "a$b"}, "foo"]}}}];
checkAggregationCounters(pipeline, {"$getField": 1, "$eq": 1}, 1);

pipeline =
    [{$match: {_id: 1, $expr: {$eq: [{$getField: {field: "a$b", input: {"a$b": "b"}}}, "b"]}}}];
checkAggregationCounters(pipeline, {"$getField": 1, "$eq": 1}, 1);

pipeline = [{
    $project: {_id: 1, test: {$setField: {field: {$const: "a.b"}, input: "$$ROOT", value: "barrr"}}}
}];
checkAggregationCounters(pipeline, {"$setField": 1, "$const": 1}, 2);

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
checkAggregationCounters(pipeline, {"$getField": 1}, 3);

initTestColl(5);
pipeline = [
    {$lookup: {
        from: testColl.getName(),
        pipeline: [{$project: {x: 1, _id: 0, test: {$getField: "a$b"}}}],
        as: "joinedField"
}}];
checkAggregationCounters(pipeline, {"$getField": 1}, 2);

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
}, {"$setField": 1});

// Expressions in view pipeline.
db.view.drop();
let viewPipeline = [{$match: {$expr: {$eq: [{$getField: "a.b"}, "bar"]}}}];
assert.commandWorked(
    db.runCommand({create: "view", viewOn: coll.getName(), pipeline: viewPipeline}));
checkCounters(() => db.view.find().itcount(), {"$getField": 1, "$eq": 1});

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
checkAggregationCounters(pipeline, {"$cond": 1, "$gte": 1}, 2);

// $ifNull
pipeline = [{$project: {description: {$ifNull: ["$description", "Unspecified"]}}}];
checkAggregationCounters(pipeline, {"$ifNull": 1}, 2);

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
checkFindCounters(query, {"$divide": 1, "$subtract": 1, "$eq": 1, "$gt": 1, "$switch": 1}, 1);

// $cmp, $exp, $abs, $range
pipeline = [{
    $project: {
        cmpField: {$cmp: ["$x", 250]},
        expField: {$exp: "$x"},
        absField: {$abs: "$x"},
        rangeField: {$range: [0, "$x", 25]}
    }
}];
checkAggregationCounters(pipeline, {"$abs": 1, "$cmp": 1, "$exp": 1, "$range": 1}, 2);

// $or
pipeline = [{$match: {$expr: {$or: [{$eq: ["$_id", 0]}, {$eq: ["$x", 1]}]}}}];
checkAggregationCounters(pipeline, {"$eq": 2, "$or": 1}, 2);

// $dateFromParts
pipeline =
    [{$project: {date: {$dateFromParts: {'year': 2021, 'month': 10, 'day': {$add: ['$x', 10]}}}}}];
checkAggregationCounters(pipeline, {"$add": 1, "$dateFromParts": 1}, 2);

// $concat
pipeline = [{$project: {mystring: {$concat: [{$getField: "a$b"}, {$getField: "a.b"}]}}}];
checkAggregationCounters(pipeline, {"$concat": 1, "$getField": 2}, 2);

// $toDouble
pipeline = [{$project: {doubleval: {$toDouble: "$_id"}}}];
checkAggregationCounters(pipeline, {"$toDouble": 1}, 2);

// $setIntersection
pipeline = [{$project: {intersection: {$setIntersection: [[1, 2, 3], [3, 2]]}}}];
checkAggregationCounters(pipeline, {"$setIntersection": 1}, 2);

// Expressions in bulk operations.
const bulkColl = db.operator_counters_expressions3;
for (let i = 0; i < 3; i++) {
    assert.commandWorked(bulkColl.insert({_id: i, x: i}));
}
const bulkOp = bulkColl.initializeUnorderedBulkOp();
bulkOp.find({$expr: {$eq: ["$x", 2]}}).update({$set: {x: 10}});
bulkOp.find({$expr: {$lt: ["$x", 1]}}).remove();
checkCounters(() => assert.commandWorked(bulkOp.execute()), {"$eq": 1, "$lt": 1});

MongoRunner.stopMongod(mongod);
})();
