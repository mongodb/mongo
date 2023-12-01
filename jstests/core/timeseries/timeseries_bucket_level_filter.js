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
import {getAggPlanStage, getEngine} from "jstests/libs/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/sbe_explain_helpers.js";

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
