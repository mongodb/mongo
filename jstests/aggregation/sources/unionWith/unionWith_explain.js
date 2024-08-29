/**
 * Test that $unionWith's pipeline argument returns the same explain as an equivalent normal
 * pipeline. The assertions in this test assume that the optimizer for $unionWith queries is
 * the same as the optimizer for the "normal" pipeline. This assumption is not strictly true when
 * CQF is enabled.
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

import {anyEq, arrayEq, documentEq} from "jstests/aggregation/extras/utils.js";
import {getAggPlanStage, getExecutionStats} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());
const collA = testDB.A;
collA.drop();
const collB = testDB.B;
collB.drop();
const collC = testDB.C;
collC.drop();
const docsPerColl = 5;
for (let i = 0; i < docsPerColl; i++) {
    assert.commandWorked(collA.insert({a: i, val: i, groupKey: i}));
    assert.commandWorked(collB.insert({b: i, val: i * 2, groupKey: i}));
    assert.commandWorked(collC.insert({c: i, val: 10 - i, groupKey: i}));
}

const executionStatsIngoredFields = [
    "executionTimeMillis",
    "executionTimeMillisEstimate",
    "saveState",
    "restoreState",
];

const stagesIgnoredFields = [
    "slots",
    "optimizationTimeMillis",
];

const mongosIgnoredFields = [
    "works",
    "needTime",
    "queryHash",
    "planCacheKey",
    "optimizationTimeMillis",
].concat(executionStatsIngoredFields, stagesIgnoredFields);

const queryPlannerIgnoredFields = [
    "optimizedPipeline",
    "optimizationTimeMillis",
].concat(stagesIgnoredFields);

function getUnionWithStage(explain) {
    if (explain.splitPipeline != null) {
        // If there is only one shard, the whole pipeline will run on that shard.
        const subAggPipe = explain.splitPipeline === null ? explain.shards["shard-rs0"].stages
                                                          : explain.splitPipeline.mergerPart;
        for (let i = 0; i < subAggPipe.length; i++) {
            const stage = subAggPipe[i];
            if (stage.hasOwnProperty("$unionWith")) {
                return stage;
            }
        }
    } else {
        return getAggPlanStage(explain, "$unionWith");
    }
}

function buildErrorString(unionExplain, realExplain, field) {
    return "Explains did not match in field " + field + ". Union:\n" + tojson(unionExplain) +
        "\nRegular:\n" + tojson(realExplain);
}

function anyEqWithIgnoredFields(union, regular, ignoredFields) {
    return anyEq(union, regular, false /* verbose */, null /* valueComparator */, ignoredFields);
}

function documentEqWithIgnoredFields(union, regular, ignoredFields) {
    return documentEq(
        union, regular, false /* verbose */, null /* valueComparator */, ignoredFields);
}

function arrayEqWithIgnoredFields(union, regular, ignoredFields) {
    return arrayEq(union, regular, false /* verbose */, null /* valueComparator */, ignoredFields);
}

function assertExplainEq(union, regular) {
    if (FixtureHelpers.isMongos(testDB)) {
        assert(
            anyEqWithIgnoredFields(union.splitPipeline, regular.splitPipeline, mongosIgnoredFields),
            buildErrorString(union, regular, "splitPipeline"));

        assert.eq(
            union.mergeType, regular.mergeType, buildErrorString(union, regular, "mergeType"));

        assert(documentEqWithIgnoredFields(union.shards, regular.shards, mongosIgnoredFields),
               buildErrorString(union, regular, "shards"));
    } else if ("executionStats" in regular) {
        const unionStats = union[0].$cursor.executionStats;
        const regularStats = regular.executionStats;

        assert(documentEqWithIgnoredFields(unionStats.executionStages,
                                           regularStats.executionStages,
                                           executionStatsIngoredFields),
               buildErrorString(unionStats, regularStats, "executionStages"));
    } else if ("stages" in regular) {
        // For explains run with the runCommand({explain: ...}) format.
        if (regular.stages.length > 1 && "$cursor" in regular.stages[0] &&
            "executionStats" in regular.stages[0]["$cursor"]) {
            assert(
                arrayEqWithIgnoredFields(union,
                                         regular.stages,
                                         [...stagesIgnoredFields, ...executionStatsIngoredFields]),
                buildErrorString(union, regular, "stages with executionStats"));
        } else {
            assert(arrayEqWithIgnoredFields(union, regular.stages, stagesIgnoredFields),
                   buildErrorString(union, regular, "stages"));
        }
    } else if ("queryPlanner" in regular) {
        assert.eq(union.length, 1, "Expected single union stage");
        const unionCursor = union[0].$cursor;
        assert(documentEqWithIgnoredFields(
                   regular.queryPlanner, unionCursor.queryPlanner, queryPlannerIgnoredFields),
               buildErrorString(unionCursor, regular, "queryPlanner"));
    } else {
        assert(false,
               "Don't know how to compare following explains.\n" +
                   "regular: " + tojson(regular) + "\n" +
                   "union: " + tojson(union) + "\n");
    }
}

function assertExplainMatch(unionExplain, regularExplain) {
    const unionStage = getUnionWithStage(unionExplain);
    assert(unionStage, unionExplain);
    const unionSubExplain = unionStage.$unionWith.pipeline;
    assertExplainEq(unionSubExplain, regularExplain);
}

function testPipeline(pipeline) {
    let unionResult = collA.aggregate([{$unionWith: {coll: collB.getName(), pipeline: pipeline}}],
                                      {explain: true});
    let queryResult = collB.aggregate(pipeline, {explain: true});
    assertExplainMatch(unionResult, queryResult);

    // Alternative explain invocation. This is a regression test for SERVER-89344.
    if (!FixtureHelpers.isMongos(db)) {
        unionResult = db.runCommand({
            explain: {
                "aggregate": collA.getName(),
                "pipeline": [{$unionWith: {coll: collB.getName(), pipeline: pipeline}}],
                "cursor": {}
            }
        });
        queryResult = db.runCommand(
            {explain: {"aggregate": collB.getName(), "pipeline": pipeline, "cursor": {}}});
        assertExplainMatch(unionResult, queryResult);
    }
}

testPipeline([{$addFields: {bump: true}}]);

testPipeline([{$group: {_id: "$groupKey", sum: {$sum: "$val"}}}]);

testPipeline([{$group: {_id: "$groupKey", sum: {$sum: "$val"}}}, {$addFields: {bump: true}}]);

testPipeline([{$unionWith: {coll: collC.getName()}}]);

testPipeline([{$unionWith: {coll: collC.getName(), pipeline: [{$addFields: {bump: true}}]}}]);

testPipeline([
    {$project: {firstProj: false}},
    {$group: {_id: "$groupKey", sum: {$sum: "$val"}}},
    {$match: {_id: 2}}
]);

testPipeline([{$limit: 3}, {$sort: {_id: 1}}, {$addFields: {bump: true}}]);

testPipeline([{
    $addFields: {
        value: {
            $function: {
                body: function(base, pow) {
                    return Math.pow(base, pow);
                },
                args: [2, 3],
                lang: "js"
            }
        }
    }
}]);

// Ensure that both agg with the explain argument and the explain command work.
assert.commandWorked(testDB.runCommand({
    aggregate: collA.getName(),
    pipeline: [{$unionWith: collB.getName()}],
    cursor: {},
    explain: true
}));
assert.commandWorked(testDB.runCommand({
    explain: {
        aggregate: collA.getName(),
        pipeline: [{$unionWith: collB.getName()}],
        cursor: {},
    }
}));

// Ensure that $unionWith can still execute explain if followed by a stage that calls dispose().
let result = assert.commandWorked(testDB.runCommand({
    explain: {
        aggregate: collA.getName(),
        pipeline: [{$unionWith: collB.getName()}, {$limit: 1}],
        cursor: {},
    }
}));

// Test that execution stats inner cursor is populated.
result = collA.explain("executionStats").aggregate([{"$unionWith": collB.getName()}]);
assert.commandWorked(result);
let expectedResult = collB.explain("executionStats").aggregate([]);
assert.commandWorked(expectedResult);
let unionStage = getUnionWithStage(result);
assert(unionStage, result);
if (FixtureHelpers.isMongos(testDB)) {
    assert(documentEqWithIgnoredFields(
               expectedResult.shards, unionStage.$unionWith.pipeline.shards, mongosIgnoredFields),
           buildErrorString(unionStage, expectedResult));
    // TODO SERVER-50597 Fix unionWith nReturned stat in sharded cluster
    // assert.eq(unionStage.nReturned, docsPerColl, unionStage);
} else {
    assert.eq(unionStage.nReturned, docsPerColl * 2, unionStage);
    assert.eq(unionStage.$unionWith.pipeline[0].$cursor.executionStats.nReturned,
              docsPerColl,
              unionStage);
}

// Test explain with executionStats when the $unionWith stage doesn't need to read from it's
// sub-pipeline.
result = collA.explain("executionStats").aggregate([{"$unionWith": collB.getName()}, {$limit: 1}]);
assert.commandWorked(result);
unionStage = getUnionWithStage(result);
assert(unionStage, result);
if (!FixtureHelpers.isSharded(collB)) {
    assert.eq(unionStage.nReturned, 1, unionStage);
    assert.eq(unionStage.$unionWith, {coll: "B", pipeline: []}, unionStage);
}

// Test explain with executionStats when the $unionWith stage partially reads from it's
// sub-pipeline.
result = collA.explain("executionStats")
             .aggregate([{"$unionWith": collB.getName()}, {$limit: docsPerColl + 1}]);
assert.commandWorked(result);
unionStage = getUnionWithStage(result);
assert(unionStage, result);
if (!FixtureHelpers.isSharded(collB)) {
    assert.eq(unionStage.nReturned, docsPerColl + 1, unionStage);
    // TODO SERVER-50597 Fix the executionStats of $unionWith sub-pipeline, the actual result should
    // be 1 instead of docsPerColl.
    const unionWithExecutionStats = unionStage.$unionWith.pipeline.shards
        ? getExecutionStats(unionStage.$unionWith.pipeline)[0]
        : unionStage.$unionWith.pipeline[0].$cursor.executionStats;
    assert.eq(unionWithExecutionStats.nReturned, docsPerColl, unionStage);
}

// Test an index scan.
const indexedColl = testDB.indexed;
assert.commandWorked(indexedColl.createIndex({val: 1}));
indexedColl.insert([{val: 0}, {val: 1}, {val: 2}, {val: 3}]);

result = collA.explain("executionStats").aggregate([
    {$unionWith: {coll: indexedColl.getName(), pipeline: [{$match: {val: {$gt: 2}}}]}}
]);
expectedResult = indexedColl.explain("executionStats").aggregate([{$match: {val: {$gt: 2}}}]);
assertExplainMatch(result, expectedResult);

// Test a nested $unionWith which itself should perform an index scan.
testPipeline([{$unionWith: {coll: indexedColl.getName(), pipeline: [{$match: {val: {$gt: 0}}}]}}]);

// Similar test as above, except the $match is pushed down to the inner pipeline as part of a
// rewrite optimization.
const res = db.adminCommand({getParameter: 1, "failpoint.disablePipelineOptimization": 1});
assert.commandWorked(res);
if (!res["failpoint.disablePipelineOptimization"].mode) {
    result = collA.explain("executionStats")
                 .aggregate([{$unionWith: indexedColl.getName()}, {$match: {val: {$gt: 2}}}]);
    expectedResult = indexedColl.explain("executionStats").aggregate([{$match: {val: {$gt: 2}}}]);
    assertExplainMatch(result, expectedResult);
}

// Test that $unionWith that has uses variables runs correctly.
result = collA.explain("executionStats").aggregate([{
    $unionWith: {
        coll: collB.getName(),
        pipeline: [
            {$match: {b: 2}},
            {$redact: {$cond: {if: {$eq: ["val", 2]}, then: "$$PRUNE", else: "$$PRUNE"}}}
        ]
    }
}]);
assert.commandWorked(result);
