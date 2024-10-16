/**
 * Tests that the time series bucket level filters are able to skip entire buckets before unpacking.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getAggPlanStage, getEngine} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";

const coll = db.timeseries_bucket_level_filter;
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "time", metaField: "meta"}}));

// Trivial, small data set with one document and one bucket.
assert.commandWorked(coll.insert({time: new Date(), meta: 1, a: 42, b: 17}));

// Test that bucket-level filters are applied on a collscan plan.
(function testBucketLevelFiltersOnCollScanPlan() {
    const pipeline = [
        {$match: {a: {$gt: 100}}},  // 'a' is never greater than 100.
        {$count: "ct"}
    ];
    const explain = coll.explain("executionStats").aggregate(pipeline);
    if (getEngine(explain) == "sbe") {
        // Ensure we get a collection scan, with one 'scan' stage.
        const scanStages = getSbePlanStages(explain, "scan");
        assert.eq(scanStages.length, 1, () => "Expected one scan stage " + tojson(explain));

        // Ensure the scan actually returned something.
        assert.gte(scanStages[0].nReturned,
                   1,
                   () => "Expected one value returned from scan " + tojson(explain));

        // Check that the ts_bucket_to_cellblock stage and its child returned 0 blocks, since
        // nothing passed the bucket level filter.
        const bucketStages = getSbePlanStages(explain, "ts_bucket_to_cellblock");
        assert.eq(bucketStages.length, 1, () => "Expected one bucket stage " + tojson(explain));

        assert.eq(bucketStages[0].nReturned,
                  0,
                  () => "Expected bucket stage to return nothing " + tojson(explain));
        assert.eq(bucketStages[0].inputStage.nReturned,
                  0,
                  () => "Expected bucket stage child to return nothing " + tojson(explain));
    } else {
        const collScanStage = getAggPlanStage(explain, "COLLSCAN");

        // The bucket-level filter attached to the COLLSCAN should have filtered out everything.
        assert.eq(0,
                  collScanStage.nReturned,
                  () => "Expected coll scan stage to return nothing " + tojson(explain));
        assert.gt(collScanStage.docsExamined,
                  0,
                  () => "Expected at least 1 doc examined by collscan stage " + tojson(explain));
    }
})();

// Test that bucket-level filters are applied at the FETCH stage.
(function testBucketLevelFiltersOnIxScanFetchPlan() {
    const pipeline = [
        // 'a' is never greater than 100, but there is a bucket with meta value meta=1. The match
        // on 'meta' should allow this to use the default {meta:1,time:1} index.
        {$match: {meta: 1, a: {$gt: 100}}},

        // For simplicity, just count all the results.
        {$count: "ct"}
    ];
    const explain = coll.explain("executionStats").aggregate(pipeline);

    if (getEngine(explain) == "sbe") {
        // Ensure we get an ixscan/fetch plan, with one ixseek stage and one seek stage.
        const seekStages = getSbePlanStages(explain, "seek");
        assert.eq(seekStages.length, 1, () => "Expected one seek stage " + tojson(explain));
        assert.eq(getSbePlanStages(explain, "ixseek").length,
                  1,
                  () => "Expected one ixseek stage " + tojson(explain));

        // Ensure the seek stage actually returned something.
        assert.gte(seekStages[0].nReturned,
                   1,
                   () => "Expected seek to have returned something " + tojson(explain));

        const bucketStages = getSbePlanStages(explain, "ts_bucket_to_cellblock");
        assert.eq(bucketStages.length, 1, () => "Expected a bucket stage " + tojson(explain));

        // Ensure that the ts_bucket_to_cellblock stage and its child returned 0 blocks, since
        // nothing passed the bucket level filter.
        assert.eq(bucketStages[0].nReturned,
                  0,
                  () => "Expected bucket stage to return 0 rows " + tojson(bucketStages[0]));
        assert.eq(bucketStages[0].inputStage.nReturned,
                  0,
                  () => "Expected bucket stage child to return 0 rows " + tojson(bucketStages[0]));
    } else {
        const ixscanStage = getAggPlanStage(explain, "IXSCAN");
        // The ixscan stage should return some data.
        assert.gt(ixscanStage.nReturned,
                  0,
                  () => "Expected ixscan stage to return at least one row " + tojson(explain));

        const fetchStage = getAggPlanStage(explain, "FETCH");
        // The fetch stage should filter out all of the buckets with the bucket-level filter.
        assert.eq(0,
                  fetchStage.nReturned,
                  () => "Expected fetch stage to return 0 rows " + tojson(explain));
    }
})();

// Test that filters over data with missing fields yield correct results (this probably means that
// the bucket-level filters couldn't be applied, but we don't care to check the plan as long as the
// results are correct).
(function testWithMissingField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    // These two events will be inserted into the same bucket.
    const event = {_id: 0, t: new Date(), m: 0, x: "abc"};
    const event_with_missing = {_id: 0, t: new Date(), m: 0};
    coll.insertMany([event, event_with_missing]);

    const testCases = [
        // Explicit comparison for null should find missing.
        {pipeline: [{$match: {x: null}}], expectedResult: [event_with_missing]},

        // Type-bracketing comparison.
        {pipeline: [{$match: {x: {$lt: "aaa"}}}], expectedResult: []},
        {pipeline: [{$match: {x: {$lte: 10}}}], expectedResult: []},

        // In non-type-bracketing comparison (inside $expr): missing == null < number < string
        {pipeline: [{$match: {$expr: {$lt: ["$x", "aaa"]}}}], expectedResult: [event_with_missing]},
        {pipeline: [{$match: {$expr: {$lte: ["$x", 10]}}}], expectedResult: [event_with_missing]},
    ];

    for (const test of testCases) {
        assertArrayEq({
            expected: test.expectedResult,
            actual: coll.aggregate(test.pipeline).toArray(),
            extraErrorMsg: ` result of $match over data with missing field. Explain: ${
                tojson(coll.explain().aggregate(test.pipeline))}`
        });
    }
})();
