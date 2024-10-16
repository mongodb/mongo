/**
 * Tests that the bucket unpacking with limit rewrite is performed and pushes the limit before
 * unpacking all buckets, while ensuring no incorrect results are created

 * @tags: [
 *     # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *     # migrations may result in writes splitting between two primaries, and thus different
 *     # buckets.
 *     does_not_support_stepdowns,
 *     tenant_migration_incompatible,
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

import {getExplainedPipelineFromAggregation} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
const metaCollName = jsTestName() + '_meta';
const metaColl = db[metaCollName];

// Helper function to set up collections.
const setupColl = (coll, collName, usesMeta) => {
    coll.drop();

    // If usesMeta is true, we want the collection to have a onlyMeta field
    if (usesMeta) {
        assert.commandWorked(
            db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
    } else {
        assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t"}}));
    }
    const bucketsColl = db.getCollection('system.buckets.' + collName);
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    let docs = [];
    // If usesMeta is true, we push 10 documents with all different onlyMeta field. This tests the
    // case when documents come from multiple different buckets. If usesMeta is false, we generate
    // 20 documents that all go into the same bucket.
    for (let i = 0; i < 10; ++i) {
        if (usesMeta) {
            docs.push({m: {"sensorId": i, "type": "temperature"}, t: new Date(i), _id: i});
        } else {
            docs.push({t: new Date(i), _id: i});
            docs.push({t: new Date(i * 10), _id: i * 10});
        }
    }
    assert.commandWorked(coll.insert(docs));
    return docs;
};

// Helper function to check the PlanStage.
const assertPlanStagesInPipeline =
    ({pipeline, expectedStages, expectedResults = [], onlyMeta = false}) => {
        // If onlyMeta is set to true, we only want to include the collection with onlyMeta field
        // specified to ensure sort can be done on the onlyMeta field
        var colls = onlyMeta ? [metaColl] : [coll, metaColl];
        for (const c of colls) {
            const aggRes = c.explain().aggregate(pipeline);
            const planStage =
                getExplainedPipelineFromAggregation(db, c, pipeline, {inhibitOptimization: false});
            // We check index at i in the PlanStage against the i'th index in expectedStages
            // Should rewrite [{$_unpack}, {$limit: x}] pipeline as [{$limit:
            // x}, {$_unpack}, {$limit: x}]
            assert(expectedStages.length == planStage.length);
            for (var i = 0; i < expectedStages.length; i++) {
                assert(planStage[i].hasOwnProperty(expectedStages[i]), tojson(aggRes));
            }

            if (expectedResults.length != 0) {
                const result = c.aggregate(pipeline).toArray();
                assert(expectedResults.length == result.length);
                for (var i = 0; i < expectedResults.length; i++) {
                    assert.docEq(result[i], expectedResults[i], tojson(result));
                }
            }
        }
    };

// Helper function to test correctness.
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
const metaDocs = setupColl(metaColl, metaCollName, true);

// Simple limit test. Because the pushed down limit is in the PlanStage now,
// getExplainedPipelineFromAggregation does not display it and we don't see the first limit / sort
// stage. The presence of the pushed limit is tested in unit tests.
assertPlanStagesInPipeline(
    {pipeline: [{$limit: 2}], expectedStages: ["$_internalUnpackBucket", "$limit"]});
// Test that when two limits are present, they get squashed into 1 taking limit of the smaller
// (tighter) value
assertPlanStagesInPipeline(
    {pipeline: [{$limit: 2}, {$limit: 10}], expectedStages: ["$_internalUnpackBucket", "$limit"]});
// Adding another stage after $limit to make sure that is also executed
assertPlanStagesInPipeline({
    pipeline: [{$limit: 2}, {$match: {"temp": 11}}],
    expectedStages: ["$_internalUnpackBucket", "$limit", "$match"]
});

// Correctness test
testLimitCorrectness(2);
testLimitCorrectness(10);
testLimitCorrectness(20);

// Test that sort absorbs the limits following it.
assertPlanStagesInPipeline({
    pipeline: [{$sort: {'m.sensorId': 1}}, {$limit: 2}],
    expectedStages: ["$_internalUnpackBucket", "$limit"],
    expectedResults: [metaDocs[0], metaDocs[1]],
    onlyMeta: true
});
assertPlanStagesInPipeline({
    pipeline: [{$sort: {"m.sensorId": -1}}, {$limit: 10}, {$limit: 2}],
    expectedStages: ["$_internalUnpackBucket", "$limit"],
    expectedResults: [metaDocs[9], metaDocs[8]],
    onlyMeta: true
});
assertPlanStagesInPipeline({
    pipeline: [{$sort: {"m.sensorId": 1}}, {$limit: 10}, {$limit: 50}],
    expectedStages: ["$_internalUnpackBucket", "$limit"],
    expectedResults: [
        metaDocs[0],
        metaDocs[1],
        metaDocs[2],
        metaDocs[3],
        metaDocs[4],
        metaDocs[5],
        metaDocs[6],
        metaDocs[7],
        metaDocs[8],
        metaDocs[9]
    ],
    onlyMeta: true
});
// Test limit comes before sort.
assertPlanStagesInPipeline({
    pipeline: [{$limit: 2}, {$sort: {"m.sensorId": 1}}],
    expectedStages: ["$_internalUnpackBucket", "$limit", "$sort"],
    onlyMeta: true
});
