/**
 * Tests the case where the data integrity check for inserting into a compressed bucket fails while
 * performing an arbitrary update on a bucket.
 *
 * @tags: [
 *   featureFlagTimeseriesUpdatesSupport,
 *   requires_fcv_80,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

TestData.testingDiagnosticsEnabled = false;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB(jsTestName());
const coll = testDB.coll;
const time = ISODate("2024-01-16T20:48:39.448Z");

// Check that smallArray is entirely contained by largeArray.
// Returns false if a member of smallArray is not in largeArray.
function arrayIsSubset(smallArray, largeArray) {
    for (let i = 0; i < smallArray.length; ++i) {
        if (!Array.contains(largeArray, smallArray[i])) {
            jsTestLog("Could not find " + smallArray[i] + " in largeArray");
            return false;
        }
    }

    return true;
}

function runIntermediateDataCheckTest(isOrdered) {
    jsTestLog("isOrdered: { " + isOrdered + " }");
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));

    assert.commandWorked(coll.insertMany([
        {t: time, m: 0, a: 1},                            // Bucket 0
        {t: new Date(time.getTime() + 100), m: 0, a: 2},  // Bucket 0
        {t: new Date(time.getTime() + 200), m: 1, a: 1},  // Bucket 1
        {t: new Date(time.getTime() + 300), m: 1, a: 2},  // Bucket 1
        {t: new Date(time.getTime() + 400), m: 2, a: 1},  // Bucket 2
        {t: new Date(time.getTime() + 500), m: 2, a: 2},  // Bucket 2
    ]));

    // Ensure that we have 3 buckets.
    assert.eq(getTimeseriesCollForRawOps(testDB, coll).find().rawData().itcount(), 3);

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
    let buckets = getTimeseriesCollForRawOps(testDB, coll).find().rawData().toArray();

    if (isOrdered) {
        // 8 Total Measurements Committed : The first 6 are from our initial inserts,
        // the next 2 are from writing the measurements from Bucket 0 to Bucket 3 as part of our
        // first update.
        assert.eq(stats.numMeasurementsCommitted, 8, tojson(stats));
        assert.eq(stats.numBucketInserts, 4, tojson(stats));
        assert.eq(stats.numBucketUpdates, 0, tojson(stats));
        assert.eq(stats.numBucketsOpenedDueToMetadata, 4, tojson(stats));
        assert.eq(stats.numBucketFetchesFailed, 0, tojson(stats));
        assert.eq(stats.numBucketQueriesFailed, 3, tojson(stats));
        TimeseriesTest.checkBucketReopeningsFailedCounters(stats, {});
        // One bucket frozen from the triggered failpoint.
        assert.eq(stats.numBucketsFrozen, 1, tojson(stats));
        // Check that we have 3 buckets and that two of them are the untouched Bucket 1
        // and Bucket 2, and that one is the newly created Bucket 3.
        assert.eq(stats.bucketCount, 3, tojson(stats));
        assert.eq(buckets.length, 3, tojson(buckets));
        assert.eq(buckets[0].control.count, 2, tojson(buckets));
        assert.eq(buckets[1].control.count, 2, tojson(buckets));
        assert.eq(buckets[2].control.count, 2, tojson(buckets));

        // We want to check that one of the meta values was replaced,
        // but cannot guarantee which one.
        let metasFound = [buckets[0].meta, buckets[1].meta, buckets[2].meta];
        let metasExpected = [0, 1, 2, "A"];
        assert(arrayIsSubset(metasFound, metasExpected), "meta values incorrect");
        assert.contains("A", metasFound);
    } else {
        // 14 Total Measurements Committed : The first 6 are from our initial inserts,
        // the next 2 are from writing the measurements from Bucket 0 to Bucket 3 as part of our
        // first update, the next 6 are from writing the measurements from Bucket 1, Bucket 2, and
        // Bucket 3 to Bucket 4 as part of our second update.
        assert.eq(stats.numMeasurementsCommitted, 14, tojson(stats));
        assert.eq(stats.numBucketInserts, 5, tojson(stats));
        assert.eq(stats.numBucketUpdates, 2, tojson(stats));
        assert.eq(stats.numBucketsOpenedDueToMetadata, 5, tojson(stats));
        assert.eq(stats.numBucketFetchesFailed, 0, tojson(stats));
        assert.eq(stats.numBucketQueriesFailed, 3, tojson(stats));
        TimeseriesTest.checkBucketReopeningsFailedCounters(stats, {});
        // One bucket frozen from the triggered failpoint.
        assert.eq(stats.numBucketsFrozen, 1, tojson(stats));
        assert.eq(stats.bucketCount, 1, tojson(stats));
        assert.eq(buckets.length, 1, tojson(buckets));
        assert.eq(buckets[0].meta, "B", tojson(buckets));
        assert.eq(buckets[0].control.count, 6, tojson(buckets));
    }
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'timeseriesDataIntegrityCheckFailureUpdate', mode: "off"}));
}

runIntermediateDataCheckTest(true);
runIntermediateDataCheckTest(false);

rst.stopSet();
