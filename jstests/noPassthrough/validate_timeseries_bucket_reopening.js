/**
 * Validate the state of time-series collections after inserting measurements into reopened buckets.
 *
 * We set the 'timeseriesIdleBucketExpiryMemoryUsageThreshold' to a low value and configure the
 * 'alwaysUseSameBucketCatalogStripe' failpoint to expedite bucket closures and increase the number
 * of buckets we reopen to insert into.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.
load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet({setParameter: {timeseriesIdleBucketExpiryMemoryUsageThreshold: 1024}});
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());
assert.commandWorked(db.dropDatabase());

if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
    rst.stopSet();
    jsTestLog(
        'Skipping test because the TimeseriesScalabilityImprovements feature flag is disabled.');
    return;
}

const collNamePrefix = db.validate_timeseries_bucket_reopening_;
const timeFieldName = 'time';
const metaFieldName1 = 'meta';
const metaFieldName2 = 'tag';
const valueFieldName = 'value';
let testCaseId = 0;

const validateBucketReopening = function(metaFieldName = null) {
    // Create collection with metaField passed in.
    let timeseriesOptions = {timeField: timeFieldName};
    if (metaFieldName != null) {
        timeseriesOptions = Object.merge(timeseriesOptions, {metaField: metaFieldName});
    }
    jsTestLog("Running validateBucketReopening() with timeseriesOptions = " +
              tojson(timeseriesOptions));

    const coll = db.getCollection(collNamePrefix + testCaseId++);
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: timeseriesOptions}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    // Insert documents with varying metaField values (if the 'metaFieldName' is specified).
    const distinctMetaValues = 10;
    const numOfPasses = 100;
    for (let i = 0; i < numOfPasses; ++i) {
        for (let j = 0; j < distinctMetaValues; ++j) {
            if (metaFieldName != null) {
                assert.commandWorked(coll.insert(
                    {[timeFieldName]: ISODate(), [metaFieldName]: j, [valueFieldName]: "a"}));
            } else {
                assert.commandWorked(
                    coll.insert({[timeFieldName]: ISODate(), [valueFieldName]: "a"}));
            }
        }
    }

    // Verify we inserted measurements into reopened buckets through the time-series stats.
    const stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    if (metaFieldName != null) {
        // The idea of this test is to insert measurements into reopened buckets so we are looking
        // to reopen more buckets than we create.
        assert.lt(stats.timeseries['bucketCount'],
                  stats.timeseries['numBucketsReopened'],
                  "Timeseries stats: " + tojson(stats));
        assert.lt(stats.timeseries['numBucketsOpenedDueToMetadata'],
                  stats.timeseries['numBucketsReopened'],
                  "Timeseries stats: " + tojson(stats));

        // The number of bucket inserts should be less than the number of bucket updates.
        assert.lt(stats.timeseries['numBucketInserts'],
                  stats.timeseries['numBucketUpdates'],
                  "Timeseries stats: " + tojson(stats));
    } else {
        // When no metaField is specified, all measurements fit in one bucket since there is no need
        // to close buckets due to metadata.
        assert.eq(stats.timeseries['bucketCount'], 1, "Timeseries stats: " + tojson(stats));
    }

    // Finally, validate the collection and ensure there are no inconsistencies.
    const res = assert.commandWorked(coll.validate());
    assert.eq(res.valid, true);
    assert.eq(res.nNonCompliantDocuments, 0);
    assert.eq(res.nInvalidDocuments, 0);
    assert.eq(res.errors.length, 0);
    assert.eq(res.warnings.length, 0);
};

// Activate failpoint to place all buckets in the same stripe in the BucketCatalog.
let fpSameStripe = configureFailPoint(db, "alwaysUseSameBucketCatalogStripe");

// Validate results with no metaField.
validateBucketReopening();

// Validate results with metaField == 'meta'.
validateBucketReopening(metaFieldName1);

// Validate results with metaField == 'tag'.
validateBucketReopening(metaFieldName2);

fpSameStripe.off();
rst.stopSet();
})();
