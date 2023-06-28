/**
 * Tests that the bucket unpacking with limit rewrite is performed and pushes the limit before
 * unpacking all buckets, while ensuring no incorrect results are created

 * @tags: [
 *     # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *     # writes splitting between two primaries, and thus different buckets.
 *     does_not_support_stepdowns,
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 *     # This complicates aggregation extraction.
 *     do_not_wrap_aggregations_in_facets,
 *     # Refusing to run a test that issues an aggregation command with explain because it may
 *     # return incomplete results if interrupted by a stepdown.
 *     does_not_support_stepdowns,
 *     requires_fcv_71
 * ]
 */

(function() {
"use strict";
load("jstests/core/timeseries/libs/timeseries.js");  // For TimeseriesTest
load("jstests/aggregation/extras/utils.js");         // For getExplainedPipelineFromAggregation

const collName = "timeseries_bucket_unpacking_with_limit";
const coll = db[collName];
const metaCollName = "timeseries_bucket_unpacking_with_limit_meta";
const metaColl = db[metaCollName];

// Helper function to set up collections
const setupColl = (coll, collName, usesMeta) => {
    coll.drop();

    // If usesMeta is true, we want the collection to have a meta field
    if (usesMeta) {
        assert.commandWorked(
            db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
    } else {
        assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t"}}));
    }
    const bucketsColl = db.getCollection('system.buckets.' + collName);
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    let docs = [];
    // If usesMeta is true, we push 10 documents with all different meta field. This tests the case
    // when documents come from multiple different buckets.
    // If usesMeta is false, we generate 20 documents that all go into the same bucket.
    for (let i = 0; i < 10; ++i) {
        if (usesMeta) {
            docs.push({m: i, t: new Date(i)});
        } else {
            docs.push({t: new Date(i)});
            docs.push({t: new Date(i * 2)});
        }
    }
    assert.commandWorked(coll.insert(docs));
};

// Helper function to check the PlanStage
const assertPlanStagesInPipeline = (pipeline, expectedStages) => {
    for (const c of [coll, metaColl]) {
        const aggRes = c.explain().aggregate(pipeline);
        const planStage =
            getExplainedPipelineFromAggregation(db, c, pipeline, {inhibitOptimization: false});
        // We check index at i in the PlanStage against the i'th index in expectedStages
        // Should rewrite [{$_unpack}, {$limit: x}] pipeline as [{$limit: x}, {$_unpack}, {$limit:
        // x}]
        for (var i = 0; i < expectedStages.length; i++) {
            assert(planStage[i].hasOwnProperty(expectedStages[i]), tojson(aggRes));
        }
    }
};

// Helper function to test correctness
const testLimitCorrectness = (size) => {
    for (const c of [coll, metaColl]) {
        const res = c.aggregate([{$limit: size}]).toArray();
        const allElements = c.find().toArray();
        // Checks that the result length is correct, and that each element is unique
        assert.eq(res.length, Math.min(size, allElements.length), tojson(res));
        assert.eq(res.length, new Set(res).size, tojson(res));
        // checks that each element in the result is actually from the collection
        for (var i = 0; i < res.length; i++) {
            assert.contains(res[i], allElements, tojson(res));
        }
    }
};

setupColl(coll, collName, false);
setupColl(metaColl, metaCollName, true);

// Simple limit test
assertPlanStagesInPipeline([{$limit: 2}], ["$limit", "$_internalUnpackBucket", "$limit"]);
// Adding another stage after $limit to make sure that is also executed
assertPlanStagesInPipeline([{$limit: 2}, {$match: {"temp": 11}}],
                           ["$limit", "$_internalUnpackBucket", "$limit", "$match"]);

// Correctness test
testLimitCorrectness(2);
testLimitCorrectness(10);
testLimitCorrectness(20);
})();
