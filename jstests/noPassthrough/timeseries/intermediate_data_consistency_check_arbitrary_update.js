/**
 * Tests the case where the data integrity check for inserting into a compressed bucket fails while
 * performing an arbitrary update on a bucket.
 *
 * @tags: [
 *   featureFlagTimeseriesAlwaysUseCompressedBuckets,
 *   featureFlagTimeseriesUpdatesSupport,
 *   requires_fcv_80,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

TestData.testingDiagnosticsEnabled = false;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB(jsTestName());
const coll = testDB.coll;
const bucketsColl = testDB.system.buckets.coll;
const time = ISODate("2024-01-16T20:48:39.448Z");

function runIntermediateDataCheckTest(isOrdered) {
    jsTestLog("isOrdered: { " + isOrdered + " }");
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    assert.commandWorked(coll.insertMany([
        {t: time, m: 0, a: 1},  // Bucket 0
        {t: time, m: 0, a: 2},  // Bucket 0
        {t: time, m: 1, a: 1},  // Bucket 1
        {t: time, m: 1, a: 2},  // Bucket 1
        {t: time, m: 2, a: 1},  // Bucket 2
        {t: time, m: 2, a: 2},  // Bucket 2
    ]));

    // Ensure that we have 3 buckets.
    assert.eq(bucketsColl.find().itcount(), 3);

    // Turn on the failpoint that causes the timeseries data integrity check to fail.
    // The failpoint is only hit when we are inserting measurements into an existing bucket. We set
    // times: 1 to cause it to only fail the first time that we enter the failpoint, i.e, when we
    // are performing the first of the two updates below and are trying to update measurements from
    // Bucket 1.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'timeseriesDataIntegrityCheckFailureUpdate', mode: {times: 1}}));

    // This command performs two updates. The first changes the metaField for the measurements
    // of all buckets to m: "A". If the update succeeded entirely, this would cause all measurements
    // to be moved to a new bucket, Bucket 3, that has a meta field of m : "A".
    //
    // We first update the measurements from Bucket 0. The updated measurements will be inserted
    // into Bucket 3 as an insert into a newly created bucket, so the applyDiff code path will not
    // be entered (it is only entered when we are inserting into an already existing compressed
    // bucket), so our failpoint will not be triggered here. When updating the measurements from
    // Bucket 1 we will encounter the failpoint, causing our update to fail. This should cause the
    // rest of our first update to fail, meaning that the measurements from Bucket 0 were written to
    // Bucket 3 and Bucket 0 is deleted, but Bucket 1 and Bucket 2 are unchanged.
    //
    // If our operation has `ordered` set to true, then this is where the update stops, and we are
    // left with three buckets: Bucket 1, Bucket 2, and Bucket 3.
    //
    // If our operation, however, has 'ordered' set to false, then upon failing the first update we
    // will try to perform the second update. The update follows the same update logic as the first,
    // attempting to set the metaField of all measurements to be "B". Because the failpoint will now
    // be turned off, updates to Bucket 1, Bucket 2, and Bucket 3 should all succeed, and all the
    // measurements from these 3 buckets should end up in a newly created Bucket 4. Bucket 1, Bucket
    // 2, and Bucket 3 should be deleted because they no longer have any measurements.
    assert.commandFailedWithCode(testDB.runCommand({
        "update": coll.getName(),
        updates:
            [{q: {}, u: {$set: {m: "A"}}, multi: true}, {q: {}, u: {$set: {m: "B"}}, multi: true}],
        ordered: isOrdered,
    }),
                                 ErrorCodes.TimeseriesBucketCompressionFailed);

    let stats = assert.commandWorked(coll.stats()).timeseries;
    let buckets = bucketsColl.find({}).toArray();

    if (isOrdered) {
        // 8 Total Measurements Committed : The first 6 are from our initial inserts,
        // the next 2 are from writing the measurements from Bucket 0 to Bucket 3 as part of our
        // first update.
        assert.eq(stats.numMeasurementsCommitted, 8);
        assert.eq(stats.numBucketInserts, 4);
        assert.eq(stats.numBucketUpdates, 0);
        assert.eq(stats.numBucketsOpenedDueToMetadata, 4);
        assert.eq(stats.numBucketFetchesFailed, 0);
        assert.eq(stats.numBucketQueriesFailed, 3);
        assert.eq(stats.numBucketReopeningsFailed, 0);
        // One bucket frozen from the triggered failpoint.
        assert.eq(stats.numBucketsFrozen, 1);
        // Check that we have 3 buckets and that two of them are the untouched Bucket 1
        // and Bucket 2, and that one is the newly created Bucket 3.
        assert.eq(stats.bucketCount, 3);
        assert.eq(buckets.length, 3);
        assert.eq(buckets[0].meta, 1);
        assert.eq(buckets[0].control.count, 2);
        assert.eq(buckets[1].meta, 2);
        assert.eq(buckets[1].control.count, 2);
        assert.eq(buckets[2].meta, "A");
        assert.eq(buckets[2].control.count, 2);
    } else {
        // 14 Total Measurements Committed : The first 6 are from our initial inserts,
        // the next 2 are from writing the measurements from Bucket 0 to Bucket 3 as part of our
        // first update, the next 6 are from writing the measurements from Bucket 1, Bucket 2, and
        // Bucket 3 to Bucket 4 as part of our second update.
        assert.eq(stats.numMeasurementsCommitted, 14);
        assert.eq(stats.numBucketInserts, 5);
        assert.eq(stats.numBucketUpdates, 2);
        assert.eq(stats.numBucketsOpenedDueToMetadata, 5);
        assert.eq(stats.numBucketFetchesFailed, 0);
        assert.eq(stats.numBucketQueriesFailed, 3);
        assert.eq(stats.numBucketReopeningsFailed, 0);
        // One bucket frozen from the triggered failpoint.
        assert.eq(stats.numBucketsFrozen, 1);
        assert.eq(stats.bucketCount, 1);
        assert.eq(buckets.length, 1);
        assert.eq(buckets[0].meta, "B");
        assert.eq(buckets[0].control.count, 6);
    }
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'timeseriesDataIntegrityCheckFailureUpdate', mode: "off"}));
}

runIntermediateDataCheckTest(true);
runIntermediateDataCheckTest(false);

rst.stopSet();
