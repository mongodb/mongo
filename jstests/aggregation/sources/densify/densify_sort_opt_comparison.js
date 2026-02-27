/**
 * Test to make sure $densify behavior is the same with and without the sort optimization.
 * @tags: [
 *   requires_fcv_53,
 *   requires_pipeline_optimization,
 *   do_not_wrap_aggregations_in_facets,
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {getExplainedPipelineFromAggregation} from "jstests/aggregation/extras/utils.js";
import {configureFailPointForAllShardsAndMongos} from "jstests/libs/fail_point_util.js";

// The test sets a failpoint on a specific mongos and expects subsequent commands to hit that same mongos.
// In case of multiple mongos cluster fixture, force every command to run against the same one.
// pinToSingleMongos due to configureFailPoint command.
TestData.pinToSingleMongos = true;

function checkResults(pipeline, expectedNumberOfStatesInPipeline) {
    configureFailPointForAllShardsAndMongos({
        conn: db.getMongo(),
        failPointName: "disablePipelineOptimization",
        failPointMode: "off",
    });
    let optimizedExplain = getExplainedPipelineFromAggregation(testDB, coll, pipeline);
    let optimizedResult = coll.aggregate(pipeline).toArray();

    configureFailPointForAllShardsAndMongos({
        conn: db.getMongo(),
        failPointName: "disablePipelineOptimization",
        failPointMode: "alwaysOn",
    });
    let nonOptimizedResults = coll.aggregate(pipeline).toArray();
    let nonOptimizedExplain = getExplainedPipelineFromAggregation(testDB, coll, pipeline);
    // This assert makes sure that $densify produces the same results, with and without the sort
    // optimization enabled.
    assert.eq(optimizedResult, nonOptimizedResults);
    // This assert is included to make sure that only test cases that consider the effects
    // of the sort optimization on aggregation pipelines that contain densify, are added
    // to this file in the future.
    assert(optimizedExplain.length === expectedNumberOfStatesInPipeline);
}
const dbName = "test";
const testDB = db.getSiblingDB(dbName);
const coll = testDB[jsTestName()];
coll.drop();

let collection = [
    {val: 0},
    {val: 5},
    {val: -10},
    {val: 100},
    {val: 20},
    {val: -50},
    {val: 30},
    {val: 300},
    {val: 150},
    {val: -300},
    {val: 75},
    {val: 500},
    {val: -220},
    {val: 430},
    {val: -90},
    {val: -500},
    {val: 1000},
    {val: 1400},
    {val: -750},
    {val: -1000},
    {val: 2000},
];
assert.commandWorked(coll.insert(collection));

// As there are no partitions, $densify preserves sort for these first five cases.
let pipeline = [{$densify: {field: "val", range: {step: 1, bounds: "full"}}}, {$sort: {val: 1}}];
checkResults(pipeline, 2);
pipeline = [{$densify: {field: "val", range: {step: 17, bounds: "full"}}}, {$sort: {val: 1}}];
checkResults(pipeline, 2);
pipeline = [{$densify: {field: "val", range: {step: 10, bounds: [-5, 111]}}}, {$sort: {val: 1}}];
checkResults(pipeline, 2);
pipeline = [{$densify: {field: "val", range: {step: 25, bounds: [-100, 381]}}}, {$sort: {val: 1}}];
checkResults(pipeline, 2);
pipeline = [{$densify: {field: "val", range: {step: 13, bounds: [600, 1728]}}}, {$sort: {val: 1}}];
checkResults(pipeline, 2);

coll.drop();
collection = [
    {val: 0, part: 1},
    {val: 20, part: 2},
    {val: -5, part: 1},
    {val: 50, part: 2},
    {val: 106, part: 1},
    {val: -50, part: 2},
    {val: 100, part: 1},
    {val: -75, part: 2},
    {val: 45, part: 1},
    {val: -28, part: 2},
    {val: 67, part: 1},
    {val: -19, part: 2},
    {val: -125, part: 1},
    {val: -500, part: 2},
    {val: 600, part: 1},
    {val: 1000, part: 2},
    {val: -1000, part: 1},
    {val: 1400, part: 2},
    {val: 3000, part: 1},
    {val: -1900, part: 2},
    {val: -2995, part: 1},
];
assert.commandWorked(coll.insert(collection));

// Sort order is preserved for partitions with non-full bounds.
pipeline = [
    {$densify: {field: "val", range: {step: 10, bounds: "partition"}, partitionByFields: ["part"]}},
    {$sort: {part: 1, val: 1}},
];
checkResults(pipeline, 2);
pipeline = [
    {$densify: {field: "val", range: {step: 7, bounds: "partition"}, partitionByFields: ["part"]}},
    {$sort: {part: 1, val: 1}},
];
checkResults(pipeline, 2);
pipeline = [
    {$densify: {field: "val", range: {step: 17, bounds: [-399, -19]}, partitionByFields: ["part"]}},
    {$sort: {part: 1, val: 1}},
];
checkResults(pipeline, 2);

// Queries with multiple stages that combine sort, preserve sort. In this case, the sort for densify
// is combined with the sort that $setWindowFields generates. The final $sort stage is not combined.
pipeline = [
    {$densify: {field: "val", range: {step: 11, bounds: "partition"}, partitionByFields: ["part"]}},
    {$setWindowFields: {partitionBy: "$part", sortBy: {"val": 1}, output: {val: {$sum: "$val"}}}},
    {$sort: {part: 1, val: 1}},
];
checkResults(pipeline, 4);

// Sort order is not preserved with partitions with full bounds.
pipeline = [
    {$densify: {field: "val", range: {step: 18, bounds: "full"}, partitionByFields: ["part"]}},
    {$sort: {val: 1}},
];
checkResults(pipeline, 3);

configureFailPointForAllShardsAndMongos({
    conn: db.getMongo(),
    failPointName: "disablePipelineOptimization",
    failPointMode: "off",
});
