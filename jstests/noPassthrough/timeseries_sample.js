/**
 * Tests inserting sample data into the time-series buckets collection. This test is for the
 * exercising the optimized $sample implementation for $_internalUnpackBucket.
 * @tags: [
 *     requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");

let conn = MongoRunner.runMongod({setParameter: {timeseriesBucketMaxCount: 100}});

// Although this test is tagged with 'requires_wiredtiger', this is not sufficient for ensuring
// that the parallel suite runs this test only on WT configurations.
if (jsTest.options().storageEngine && jsTest.options().storageEngine !== "wiredTiger") {
    jsTest.log("Skipping test on non-WT storage engine: " + jsTest.options().storageEngine);
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = jsTestName();
let testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

if (!TimeseriesTest.timeseriesCollectionsEnabled(testDB.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const nBuckets = 40;

const timeFieldName = "time";
const metaFieldName = "m";

let assertUniqueDocuments = function(docs) {
    let seen = new Set();
    docs.forEach(doc => {
        assert.eq(seen.has(doc._id), false);
        seen.add(doc._id);
    });
};

let assertPlanForSample = (explainRes, backupPlanSelected) => {
    // The trial stage should always appear in the output, regardless of which plan won.
    assert(aggPlanHasStage(explainRes, "TRIAL"), explainRes);

    if (backupPlanSelected) {
        assert(aggPlanHasStage(explainRes, "UNPACK_BUCKET"), explainRes);
        assert(!aggPlanHasStage(explainRes, "$_internalUnpackBucket"));
        assert(aggPlanHasStage(explainRes, "$sample"));

        // Verify that execution stats are reported correctly for the UNPACK_BUCKET stage in
        // explain.
        const unpackBucketStage = getAggPlanStage(explainRes, "UNPACK_BUCKET");
        assert.neq(unpackBucketStage, null, explainRes);
        assert(unpackBucketStage.hasOwnProperty("nBucketsUnpacked"));
        // In the top-k plan, all of the buckets need to be unpacked.
        assert.eq(unpackBucketStage.nBucketsUnpacked, nBuckets, unpackBucketStage);
    } else {
        // When the trial plan succeeds, any data produced during the trial period will be queued
        // and returned via the QUEUED_DATA stage. If the trial plan being assessed reached EOF,
        // then we expect only a QUEUED_DATA stage to appear in explain because all of the necessary
        // data has already been produced. If the plan is not EOF, then we expect OR
        // (QUEUED_DATA, <trial plan>). Either way, the presence of the QUEUED_DATA stage indicates
        // that the trial plan was selected over the backup plan.
        assert(aggPlanHasStage(explainRes, "QUEUED_DATA"), explainRes);
        assert(!aggPlanHasStage(explainRes, "$_internalUnpackBucket"));
        assert(!aggPlanHasStage(explainRes, "$sample"));
    }
};

/**
 * Creates the collection 'coll' as a time-series collection, and inserts data such that there are
 * the given number of measurementsPerBucket (assuming there are 'nBuckets'). Returns the total
 * number of measurement documents inserted into the collection.
 */
function fillBuckets(coll, measurementsPerBucket) {
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    const bucketsColl = testDB.getCollection("system.buckets." + coll.getName());
    assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

    let numDocs = nBuckets * measurementsPerBucket;
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert(
            {_id: ObjectId(), [timeFieldName]: ISODate(), [metaFieldName]: i % nBuckets, x: i});
    }
    assert.commandWorked(bulk.execute());

    let buckets = bucketsColl.find().toArray();
    assert.eq(nBuckets, buckets.length, buckets);

    return numDocs;
}

let runSampleTests = (measurementsPerBucket, backupPlanSelected) => {
    const coll = testDB.getCollection("timeseries_sample");
    coll.drop();

    let numDocs = fillBuckets(coll, measurementsPerBucket);

    // Check the time-series view to make sure we have the correct number of docs and that there are
    // no duplicates after sampling.
    const viewDocs = coll.find({}, {x: 1}).toArray();
    assert.eq(numDocs, viewDocs.length, viewDocs);

    let sampleSize = 20;
    let result = coll.aggregate([{$sample: {size: sampleSize}}]).toArray();
    assert.eq(sampleSize, result.length, result);
    assertUniqueDocuments(result);

    // Check that we have executed the correct branch of the TrialStage.
    const optimizedSamplePlan =
        coll.explain("executionStats").aggregate([{$sample: {size: sampleSize}}]);
    assertPlanForSample(optimizedSamplePlan, backupPlanSelected);

    // Run an agg pipeline with optimization disabled.
    result = coll.aggregate([{$_internalInhibitOptimization: {}}, {$sample: {size: 1}}]).toArray();
    assert.eq(1, result.length, result);

    // Check that $sample hasn't been absorbed by $_internalUnpackBucket when the
    // sample size is sufficiently large. The server will never try to use random cursor-based
    // sampling for timeseries collections when the requested sample exceeds 1% of the maximum
    // measurement count. Since the maximum number of measurements per bucket is 100, this means
    // that we expect to use a top-k plan (without using 'TrialStage') when the sample size exceeds
    // 'nBuckets'.
    sampleSize = nBuckets + 10;
    const unoptimizedSamplePlan = coll.explain().aggregate([{$sample: {size: sampleSize}}]);
    let bucketStage = getAggPlanStage(unoptimizedSamplePlan, "$_internalUnpackBucket");
    assert.neq(bucketStage, null, unoptimizedSamplePlan);
    assert.eq(bucketStage["$_internalUnpackBucket"]["sample"], undefined);
    assert(aggPlanHasStage(unoptimizedSamplePlan, "$sample"));
    assert(!aggPlanHasStage(unoptimizedSamplePlan, "TRIAL"));

    const unoptimizedResult = coll.aggregate([{$sample: {size: sampleSize}}]).toArray();
    assert.eq(Math.min(sampleSize, numDocs), unoptimizedResult.length, unoptimizedResult);
    assertUniqueDocuments(unoptimizedResult);

    // Check that a sampleSize greater than the number of measurements doesn't cause an infinte
    // loop.
    result = coll.aggregate([{$sample: {size: numDocs + 1}}]).toArray();
    assert.eq(numDocs, result.length, result);

    // Check that $lookup against a time-series collection doesn't cache inner pipeline results if
    // it contains a $sample stage.
    result =
        coll.aggregate(
                {$lookup: {from: coll.getName(), as: "docs", pipeline: [{$sample: {size: 1}}]}})
            .toArray();

    // Each subquery should be an independent sample by checking that we didn't sample the same
    // document repeatedly. It's sufficient for now to make sure that the seen set contains at least
    // two distinct samples.
    let seen = new Set();
    result.forEach(r => {
        assert.eq(r.docs.length, 1);
        seen.add(r.docs[0]._id);
    });
    assert.gte(seen.size, 2);
};

// Test the case where the buckets are only 1% full. Due to the mostly empty buckets, we expect to
// fall back to the non-optimized top-k algorithm for sampling from a time-series collection.
runSampleTests(1, true);

// Test the case where the buckets are 95% full. Here we expect the optimized
// SAMPLE_FROM_TIMESERIES_BUCKET plan to be used.
runSampleTests(95, false);

// Restart the mongod in order to raise the maximum bucket size to 1000.
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({setParameter: {timeseriesBucketMaxCount: 1000}});
testDB = conn.getDB(dbName);
const coll = testDB.getCollection("timeseries_sample");

// Create a timeseries collection that has 40 buckets, each with 900 documents.
const measurementsPerBucket = 900;
let numDocs = fillBuckets(coll, measurementsPerBucket);
assert.eq(numDocs, measurementsPerBucket * nBuckets);

// Run a sample query where the sample size is large enough to merit multiple batches.
assert.eq(150, coll.aggregate([{$sample: {size: 150}}]).itcount());

// Explain the $sample. Given that the buckets are mostly full, we expect the trial to succeed. We
// should see a TRIAL stage, and it should have selected the SAMPLE_FROM_TIMESERIES_BUCKET plan. The
// initial batch of data collected during the trial period will be returned via a QUEUED_DATA_STAGE.
const explainRes = coll.explain("executionStats").aggregate([{$sample: {size: 150}}]);
const trialStage = getAggPlanStage(explainRes, "TRIAL");
assert.neq(trialStage, null, explainRes);
const orStage = getPlanStage(trialStage, "OR");
assert.neq(orStage, null, explainRes);
const queuedDataStage = getPlanStage(orStage, "QUEUED_DATA");
assert.neq(queuedDataStage, null, explainRes);

// Verify that the SAMPLE_FROM_TIMESERIES_BUCKET stage exists in the plan and has reasonable
// runtime stats.
const sampleFromBucketStage = getPlanStage(orStage, "SAMPLE_FROM_TIMESERIES_BUCKET");
assert.neq(sampleFromBucketStage, null, explainRes);
assert(sampleFromBucketStage.hasOwnProperty("nBucketsDiscarded"), sampleFromBucketStage);
assert.gte(sampleFromBucketStage.nBucketsDiscarded, 0, sampleFromBucketStage);
assert(sampleFromBucketStage.hasOwnProperty("dupsDropped"), sampleFromBucketStage);
assert.gte(sampleFromBucketStage.dupsDropped, 0, sampleFromBucketStage);
// Since we are returning a sample size of 150, we expect to test at least that many dups.
assert(sampleFromBucketStage.hasOwnProperty("dupsTested"));
assert.gte(sampleFromBucketStage.dupsTested, 150, sampleFromBucketStage);

// The SAMPLE_FROM_TIMESERIES_BUCKET stage reads from a MULTI_ITERATOR stage, which in turn reads
// from a storage-provided random cursor.
const multiIteratorStage = getPlanStage(sampleFromBucketStage, "MULTI_ITERATOR");
assert.neq(multiIteratorStage, null, explainRes);

MongoRunner.stopMongod(conn);
})();
