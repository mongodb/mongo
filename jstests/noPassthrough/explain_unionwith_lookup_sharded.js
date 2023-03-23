/**
 * Test that explain of $lookup and $unionWith works correctly when the involved collections are
 * sharded.
 *
 * This test was originally designed to reproduce SERVER-71636.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const dbName = "test";

const st = new ShardingTest({shards: 2});
const db = st.s.getDB(dbName);

const outerColl = db["outer"];
const innerColl = db["inner"];

(function createOuterColl() {
    outerColl.drop();
    assert.commandWorked(outerColl.insert([{_id: 1, x: "foo"}, {_id: 3, x: "foo"}]));
}());

function createInnerColl() {
    innerColl.drop();
    assert.commandWorked(
        innerColl.insert([{_id: 1, x: "foo", y: "a"}, {_id: 2, x: "foo", y: "z"}]));
}
createInnerColl();

function explainStage(stage, stageName) {
    const pipeline = [stage];
    let explain = outerColl.explain("executionStats").aggregate(pipeline);
    let stageExplain = getAggPlanStage(explain, stageName);
    assert.neq(stageExplain, null, explain);
    return stageExplain;
}

// Explain of $lookup when neither collection is sharded.
const lookupStage =
        {
            $lookup: {
                from: innerColl.getName(),
                let: {
                    myX: "$x",
                },
                pipeline: [
                    {$match: {$expr: {$eq: ["$x", "$$myX"]}}},
                ],
                as: "as"
            }
        };
let stageExplain = explainStage(lookupStage, "$lookup");
assert.eq(stageExplain.nReturned, 2, stageExplain);
// The two documents in the inner collection are scanned twice, leading to four docs examined in
// total.
assert.eq(stageExplain.collectionScans, 2, stageExplain);
assert.eq(stageExplain.totalDocsExamined, 4, stageExplain);
assert.eq(stageExplain.totalKeysExamined, 0, stageExplain);
assert.eq(stageExplain.indexesUsed, [], stageExplain);

const unionWithStage = {
    $unionWith: {
        coll: innerColl.getName(),
        pipeline: [
            {$match: {$expr: {$eq: ["$x", "foo"]}}},
        ]
    }
};

const nestedUnionWithStage = {
    $unionWith: {coll: innerColl.getName(), pipeline: [unionWithStage]}
};

// Explain of $unionWith when neither collection is sharded.
stageExplain = explainStage(unionWithStage, "$unionWith");
assert.eq(stageExplain.nReturned, 4, stageExplain);

// Explain of nested $unionWith when neither collection is sharded.
stageExplain = explainStage(nestedUnionWithStage, "$unionWith");
assert.eq(stageExplain.nReturned, 6, stageExplain);

// Shard the inner collection.
assert.commandWorked(innerColl.createIndex({y: 1, x: 1}));
st.shardColl(innerColl.getName(),
             {y: 1, x: 1} /* shard key */,
             {y: "b", x: "b"} /* split at */,
             {y: "c", x: "c"} /* move */,
             dbName,
             true);

// Explain of $lookup when outer collection is unsharded and inner collection is sharded.
stageExplain = explainStage(lookupStage, "$lookup");
assert.eq(stageExplain.nReturned, 2, stageExplain);
// Now that the inner collection is sharded, the execution of the $lookup requires dispatching
// commands across the wire for the inner collection. The runtime stats currently do not reflect the
// work done by these dispatched subcommands. We could improve this in the future to more accurately
// reflect docs examined, keys examined, collection scans, etc accrued when executing the
// subpipeline.
assert.eq(stageExplain.totalDocsExamined, 0, stageExplain);
assert.eq(stageExplain.totalKeysExamined, 0, stageExplain);
assert.eq(stageExplain.collectionScans, 0, stageExplain);
assert.eq(stageExplain.indexesUsed, [], stageExplain);

// Explain of $unionWith when outer collection is unsharded and inner collection is sharded.
stageExplain = explainStage(unionWithStage, "$unionWith");
assert.eq(stageExplain.nReturned, 4, stageExplain);
// The $unionWith explain format currently shows explains for the inner pipeline from both
// targeted shards.
assert(stageExplain.$unionWith.hasOwnProperty("pipeline"), stageExplain);
const pipelineExplain = stageExplain.$unionWith.pipeline;
assert(pipelineExplain.hasOwnProperty("shards"), stageExplain);
const shardNames = Object.keys(pipelineExplain.shards);
assert.eq(shardNames.length, 2, stageExplain);
// Each shard should have returned one document.
assert.eq(pipelineExplain.shards[shardNames[0]].executionStats.nReturned, 1, stageExplain);
assert.eq(pipelineExplain.shards[shardNames[1]].executionStats.nReturned, 1, stageExplain);

// Explain of nested $unionWith when outer collection is unsharded and inner collection is sharded.
stageExplain = explainStage(nestedUnionWithStage, "$unionWith");
assert.eq(stageExplain.nReturned, 6, stageExplain);

// Shard the outer collection.
st.shardColl(outerColl.getName(),
             {_id: 1} /* shard key */,
             {_id: 2} /* split at */,
             {_id: 3} /* move */,
             dbName,
             true);

// A variant of 'explainStage()' when the stage is expected to appear twice because it runs on
// two shards.
function explainStageTwoShards(stage, stageName) {
    const pipeline = [stage];
    let explain = outerColl.explain("executionStats").aggregate(pipeline);
    let stageExplain = getAggPlanStages(explain, stageName);
    assert.eq(stageExplain.length, 2, stageExplain);
    return stageExplain;
}

// Explain of $lookup when inner and outer collections are both sharded.
stageExplain = explainStageTwoShards(lookupStage, "$lookup");
for (let explain of stageExplain) {
    assert.eq(explain.nReturned, 1, stageExplain);
    // As above, the inner collection is sharded. We don't currently ship execution stats across the
    // wire alongside the query results themselves. As a result, the docs examined, total keys
    // examined, etc. will currently always be reported as zero when the inner collection is
    // sharded. We could improve this in the future to report the stats more accurately.
    assert.eq(explain.totalDocsExamined, 0, stageExplain);
    assert.eq(explain.totalKeysExamined, 0, stageExplain);
    assert.eq(explain.collectionScans, 0, stageExplain);
    assert.eq(explain.indexesUsed, [], stageExplain);
}

// Asserts that 'explain' is for a split pipeline with an empty shards part and a merger part with
// two stages. Asserts that the first merging stage is a $mergeCursors and then returns the second
// stage in the merging pipeline.
function getStageFromMergerPart(explain) {
    assert(explain.hasOwnProperty("splitPipeline"));
    assert(explain.splitPipeline.hasOwnProperty("shardsPart"));
    assert.eq(explain.splitPipeline.shardsPart, [], explain);
    assert(explain.splitPipeline.hasOwnProperty("mergerPart"));
    let mergerPart = explain.splitPipeline.mergerPart;
    assert.eq(mergerPart.length, 2, explain);
    assert(mergerPart[0].hasOwnProperty("$mergeCursors"), explain);
    return mergerPart[1];
}

function assertStageDoesNotHaveRuntimeStats(stageExplain) {
    assert(!stageExplain.hasOwnProperty("nReturned"), stageExplain);
    assert(!stageExplain.hasOwnProperty("totalDocsExamined"), stageExplain);
    assert(!stageExplain.hasOwnProperty("totalKeysExamined"), stageExplain);
    assert(!stageExplain.hasOwnProperty("collectionScans"), stageExplain);
    assert(!stageExplain.hasOwnProperty("indexesUsed"), stageExplain);
}

// Explain of $unionWith when inner and outer collections are both sharded. We expect the $unionWith
// to be part of the merging pipeline rather than pushed down to the shards.
let explain = outerColl.explain("executionStats").aggregate([unionWithStage]);
stageExplain = getStageFromMergerPart(explain);
assert(stageExplain.hasOwnProperty("$unionWith"), explain);
assertStageDoesNotHaveRuntimeStats(stageExplain);

// Nested $unionWith when inner and outer collections are sharded.
explain = outerColl.explain("executionStats").aggregate([nestedUnionWithStage]);
stageExplain = getStageFromMergerPart(explain);
assert(stageExplain.hasOwnProperty("$unionWith"), explain);
assertStageDoesNotHaveRuntimeStats(stageExplain);

// Drop and recreate the inner collection. Re-test when the outer collection is sharded but the
// inner collection is unsharded.
createInnerColl();

// Explain of $lookup when outer collection is sharded and inner collection is unsharded. In this
// case we expect the $lookup operation to execute on the primary shard as part of the merging
// pipeline.
explain = outerColl.explain("executionStats").aggregate([lookupStage]);
stageExplain = getStageFromMergerPart(explain);
assert(stageExplain.hasOwnProperty("$lookup"), explain);
assertStageDoesNotHaveRuntimeStats(stageExplain);

// Explain of $unionWith when the outer collection is sharded and the inner collection is unsharded.
explain = outerColl.explain("executionStats").aggregate([unionWithStage]);
stageExplain = getStageFromMergerPart(explain);
assert(stageExplain.hasOwnProperty("$unionWith"), explain);
assertStageDoesNotHaveRuntimeStats(stageExplain);

// Nested $unionWith when the outer collection is sharded and the inner collection is unsharded.
explain = outerColl.explain("executionStats").aggregate([nestedUnionWithStage]);
stageExplain = getStageFromMergerPart(explain);
assert(stageExplain.hasOwnProperty("$unionWith"), explain);
assertStageDoesNotHaveRuntimeStats(stageExplain);

st.stop();
}());
