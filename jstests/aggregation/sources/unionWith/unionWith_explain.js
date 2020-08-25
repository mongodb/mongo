/**
 * Test that $unionWith's pipeline argument returns the same explain as an equivalent normal
 * pipeline.
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   sbe_incompatible,
 * ]
 */

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // arrayEq, documentEq
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.
load("jstests/libs/analyze_plan.js");         // For getAggPlanStage.

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

function docEqWithIgnoredFields(union, regular) {
    return documentEq(union, regular, false /* verbose */, null /* valueComparator */, [
        "executionTimeMillis",
        "executionTimeMillisEstimate",
        "saveState",
        "restoreState",
        "works",
        "needTime",
    ]);
}

function assertExplainEq(unionExplain, regularExplain) {
    const unionStage = getUnionWithStage(unionExplain);
    assert(unionStage);
    const unionSubExplain = unionStage.$unionWith.pipeline;
    if (FixtureHelpers.isMongos(testDB)) {
        const splitPipe = unionExplain.splitPipeline;
        if (splitPipe === null) {
            assert.eq(unionSubExplain.splitPipeline,
                      regularExplain.splitPipeline,
                      buildErrorString(unionSubExplain, regularExplain, "splitPipeline"));
        } else {
            assert(
                docEqWithIgnoredFields(unionSubExplain.splitPipeline, regularExplain.splitPipeline),
                buildErrorString(unionSubExplain, regularExplain, "splitPipeline"));
        }
        assert.eq(unionSubExplain.mergeType,
                  regularExplain.mergeType,
                  buildErrorString(unionSubExplain, regularExplain, "mergeType"));
        assert(docEqWithIgnoredFields(unionSubExplain.shards, regularExplain.shards),
               buildErrorString(unionSubExplain, regularExplain, "shards"));
    } else {
        if ("executionStats" in unionSubExplain[0].$cursor) {
            const unionSubStats =
                unionStage.$unionWith.pipeline[0].$cursor.executionStats.executionStages;
            const realStats = regularExplain.executionStats.executionStages;
            assert(docEqWithIgnoredFields(unionSubStats, realStats),
                   buildErrorString(unionSubStats, realStats));
        } else {
            const realExplain = regularExplain.stages;
            assert(arrayEq(unionSubExplain, realExplain),
                   buildErrorString(unionSubExplain, realExplain));
        }
    }
}

function testPipeline(pipeline) {
    let unionResult = collA.aggregate([{$unionWith: {coll: collB.getName(), pipeline: pipeline}}],
                                      {explain: true});
    let queryResult = collB.aggregate(pipeline, {explain: true});
    assertExplainEq(unionResult, queryResult);
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
    assert(docEqWithIgnoredFields(expectedResult.shards, unionStage.$unionWith.pipeline.shards),
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
    assert.eq(unionStage.$unionWith.pipeline[0].$cursor.executionStats.nReturned,
              docsPerColl,
              unionStage);
}

// Test an index scan.
const indexedColl = testDB.indexed;
assert.commandWorked(indexedColl.createIndex({val: 1}));
indexedColl.insert([{val: 0}, {val: 1}, {val: 2}, {val: 3}]);

result = collA.explain("executionStats").aggregate([
    {$unionWith: {coll: indexedColl.getName(), pipeline: [{$match: {val: {$gt: 2}}}]}}
]);
expectedResult = indexedColl.explain("executionStats").aggregate([{$match: {val: {$gt: 2}}}]);
assertExplainEq(result, expectedResult);

// Test a nested $unionWith which itself should perform an index scan.
testPipeline([{$unionWith: {coll: indexedColl.getName(), pipeline: [{$match: {val: {$gt: 0}}}]}}]);
})();
