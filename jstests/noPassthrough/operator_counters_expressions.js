/**
 * Tests aggregate expression counters.
 * Initially limited to $getField and $setField.
 * @tags: [requires_fcv_50]
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

function checkCounterIncrement(command, exprCounter) {
    const origCounter = db.serverStatus().metrics.operatorCounters.expressions[exprCounter];
    command();
    assert.gt(db.serverStatus().metrics.operatorCounters.expressions[exprCounter], origCounter);
}

// Find.
checkCounterIncrement(
    () => assert.eq(3, coll.find({$expr: {$eq: [{$getField: "a$b"}, "foo"]}}).itcount()),
    "$getField");

// Update.
checkCounterIncrement(
    () => assert.commandWorked(
        coll.update({_id: 0, $expr: {$eq: [{$getField: {field: "a$b", input: "$$ROOT"}}, "foo"]}},
                    {$set: {y: 10}})),
    "$getField");

checkCounterIncrement(
    () => assert.commandWorked(coll.update(
        {_id: 1}, [{$replaceWith: {$setField: {field: "a.b", input: "$$ROOT", value: "qqq"}}}])),
    "$setField");

checkCounterIncrement(() => assert.commandWorked(db.runCommand({
    update: coll.getName(),
    updates: [{
        q: {_id: 1},
        u: [{$replaceWith: {$setField: {field: "a.b", input: "$$ROOT", value: "zzz"}}}],
        upsert: false
    }]
})),
                      "$setField");

// Delete.
checkCounterIncrement(() => assert.commandWorked(db.runCommand({
    delete: coll.getName(),
    deletes: [{q: {"_id": {$gt: 1}, $expr: {$eq: [{$getField: "a$b"}, "foo"]}}, limit: 1}]
})),
                      "$getField");

// In aggregation pipeline.
let pipeline = [{$project: {_id: 1, test: {$getField: "a$b"}}}];
checkCounterIncrement(() => assert.eq(2, coll.aggregate(pipeline).itcount()), "$getField");

pipeline = [{$match: {_id: 1, $expr: {$eq: [{$getField: "a$b"}, "foo"]}}}];
checkCounterIncrement(() => assert.eq(1, coll.aggregate(pipeline).itcount()), "$getField");

pipeline =
    [{$match: {_id: 1, $expr: {$eq: [{$getField: {field: "a$b", input: {"a$b": "b"}}}, "b"]}}}];
checkCounterIncrement(() => assert.eq(1, coll.aggregate(pipeline).itcount()), "$getField");

pipeline = [{
    $project: {_id: 1, test: {$setField: {field: {$const: "a.b"}, input: "$$ROOT", value: "barrr"}}}
}];
checkCounterIncrement(() => assert.eq(2, coll.aggregate(pipeline).itcount()), "$setField");

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
checkCounterIncrement(() => assert.eq(3, coll.aggregate(pipeline).itcount()), "$getField");

initTestColl(5);
pipeline = [
    {$lookup: {
        from: testColl.getName(),
        pipeline: [{$project: {x: 1, _id: 0, test: {$getField: "a$b"}}}],
        as: "joinedField"
}}];
checkCounterIncrement(() => assert.eq(2, coll.aggregate(pipeline).itcount()), "$getField");

initTestColl(1);
let mergePipeline =
    [{$project: {test: {$setField: {field: "a$b", input: "$$ROOT", value: "merged"}}}}];
pipeline = [{
    $merge:
        {into: testColl.getName(), on: "_id", whenMatched: mergePipeline, whenNotMatched: "insert"}
}];
checkCounterIncrement(() => {
    coll.aggregate(pipeline).itcount();
    assert.eq(2, testColl.find().itcount());
}, "$setField");

// Expressions in view pipeline.
db.view.drop();
let viewPipeline = [{$match: {$expr: {$eq: [{$getField: "a.b"}, "bar"]}}}];
assert.commandWorked(
    db.runCommand({create: "view", viewOn: coll.getName(), pipeline: viewPipeline}));
checkCounterIncrement(() => db.view.find().itcount(), "$getField");

// Expressions in document validator.
const initCounter = db.serverStatus().metrics.operatorCounters.expressions["$getField"];
initTestColl(1);
assert.commandWorked(db.runCommand(
    {"collMod": testColl.getName(), "validator": {$expr: {$eq: [{$getField: "a$b"}, "new"]}}}));
const validatorCounter = db.serverStatus().metrics.operatorCounters.expressions["$getField"];
assert.gt(validatorCounter, initCounter);

// Expression counter is not incremented for each validated document.
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
assert.eq(validatorCounter, db.serverStatus().metrics.operatorCounters.expressions["$getField"]);

MongoRunner.stopMongod(mongod);
})();
