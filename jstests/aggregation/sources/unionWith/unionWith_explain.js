/**
 * Test that $unionWith's pipeline argument returns the same explain as an equivalent normal
 * pipeline.
 * @tags: [do_not_wrap_aggregations_in_facets]
 */

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // arrayEq, documentEq
load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.

const testDB = db.getSiblingDB(jsTestName());
const collA = testDB.A;
collA.drop();
const collB = testDB.B;
collB.drop();
const collC = testDB.C;
collC.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(collA.insert({a: i, val: i, groupKey: i}));
    assert.commandWorked(collB.insert({b: i, val: i * 2, groupKey: i}));
    assert.commandWorked(collC.insert({c: i, val: 10 - i, groupKey: i}));
}
function getUnionWithStage(pipeline) {
    for (let i = 0; i < pipeline.length; i++) {
        const stage = pipeline[i];
        if (stage.hasOwnProperty("$unionWith")) {
            return stage;
        }
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
    if (FixtureHelpers.isMongos(testDB)) {
        const splitPipe = unionExplain.splitPipeline;
        // If there is only one shard, the whole pipeline will run on that shard.
        const subAggPipe =
            splitPipe === null ? unionExplain.shards["shard-rs0"].stages : splitPipe.mergerPart;
        const unionStage = getUnionWithStage(subAggPipe);
        const unionSubExplain = unionStage.$unionWith.pipeline;
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
        const unionStage = getUnionWithStage(unionExplain.stages);
        const unionSubExplain = unionStage.$unionWith.pipeline;
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
var result = assert.commandWorked(testDB.runCommand({
    explain: {
        aggregate: collA.getName(),
        pipeline: [{$unionWith: collB.getName()}, {$limit: 1}],
        cursor: {},
    }
}));

// Test that execution stats inner cursor is populated.
result = collA.explain("executionStats").aggregate([{"$unionWith": collB.getName()}]);
var expectedResult = collB.explain("executionStats").aggregate([]);
assert(result.ok, result);
assert(expectedResult.ok, result);
// If we attached a fresh cursor stage, the number returned would still be zero.
if (FixtureHelpers.isMongos(testDB)) {
    if (result.splitPipeline != null) {
        const pipeline = result.splitPipeline.mergerPart;
        const unionStage = getUnionWithStage(pipeline);
        assert(docEqWithIgnoredFields(expectedResult.shards, unionStage.$unionWith.pipeline.shards),
               buildErrorString(unionStage, expectedResult));
    }
} else {
    assert(result.stages[1].$unionWith.pipeline[0].$cursor.executionStats.nreturned != 0, result);
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
})();
