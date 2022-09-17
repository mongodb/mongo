/**
 * Inserts time-series measurements into closed buckets identified by query-based reopening method.
 * * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This test depends on stats read from the primary node in replica sets.
 *   assumes_read_preference_unchanged,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
    return;
}

const coll = db.timeseries_reopened_bucket_insert;
const bucketsColl = db["system.buckets." + coll.getName()];
const timeField = "time";
const metaField = "mm";

const resetCollection = function() {
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));
};

const checkIfBucketReopened = function(
    measurement, willCreateBucket = false, willReopenBucket = false) {
    let stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    const prevBucketCount = stats.timeseries['bucketCount'];
    const prevExpectedReopenedBuckets = stats.timeseries['numBucketsReopened'];

    const expectedReopenedBuckets =
        (willReopenBucket) ? prevExpectedReopenedBuckets + 1 : prevExpectedReopenedBuckets;
    const expectedBucketCount = (willCreateBucket) ? prevBucketCount + 1 : prevBucketCount;
    assert.commandWorked(coll.insert(measurement));

    stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    assert.eq(stats.timeseries['bucketCount'], expectedBucketCount);
    assert.eq(stats.timeseries['numBucketsReopened'], expectedReopenedBuckets);
};

const expectNoBucketReopening = function() {
    jsTestLog("Entering expectNoBucketReopening...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2022-08-26T19:17:00Z"),
        [metaField]: "bucket1",
    };
    const measurement2 = {
        [timeField]: ISODate("2022-08-26T19:18:00Z"),
        [metaField]: "bucket1",
    };

    // When there are no open buckets available and none to reopen, we expect to create a new one.
    checkIfBucketReopened(measurement1, /* willCreateBucket */ true, /* willReopenBucket */ false);
    // We don't expect buckets to be created or reopened when a suitable, open bucket exists.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ false, /* willReopenBucket */ false);

    jsTestLog("Exiting expectNoBucketReopening.");
}();

const expectToReopenBuckets = function() {
    jsTestLog("Entering expectToReopenBuckets...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "reopenedBucket1",
    };
    const measurement2 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "reopenedBucket1",
    };
    const measurement3 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "reopenedBucket2",
    };

    const bucketDoc = {
        "_id": ObjectId("63091c2c050b7495eaef4580"),
        "control": {
            "version": 1,
            "min": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:30Z")
            },
            "closed": false
        },
        "meta": "reopenedBucket1",
        "data": {
            "_id": {"0": ObjectId("63091c30138e9261fd70a903")},
            "time": {"0": ISODate("2022-08-26T19:19:30Z")}
        }
    };
    const missingClosedFlagBucketDoc = {
        "_id": ObjectId("63091c2c050b7495eaef4581"),
        "control": {
            "version": 1,
            "min": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:30Z")
            },
        },
        "meta": "reopenedBucket2",
        "data": {
            "_id": {"0": ObjectId("63091c30138e9261fd70a903")},
            "time": {"0": ISODate("2022-08-26T19:19:30Z")}
        }
    };

    // Insert closed bucket into the system.buckets collection.
    assert.commandWorked(bucketsColl.insert(bucketDoc));

    checkIfBucketReopened(measurement1, /* willCreateBucket */ false, /* willReopenBucket */ true);
    // Now that we reopened 'bucketDoc' we shouldn't have to open a new bucket.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ false, /* willReopenBucket */ false);

    // Insert closed bucket into the system.buckets collection.
    assert.commandWorked(bucketsColl.insert(missingClosedFlagBucketDoc));
    // We expect to reopen buckets with missing 'closed' flags (this means the buckets are open for
    // inserts).
    checkIfBucketReopened(measurement3, /* willCreateBucket */ false, /* willReopenBucket */ true);

    jsTestLog("Exiting expectToReopenBuckets.");
}();

const failToReopenNonSuitableBuckets = function() {
    jsTestLog("Entering failToReopenNonSuitableBuckets...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "NonSuitableBucket1",
    };
    const measurement2 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "NonSuitableBucket2",
    };
    const measurement3 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "NonSuitableBucket3",
    };
    const measurement4 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "NonSuitableBucket4",
    };
    const measurement5 = {
        [timeField]: ISODate("2022-08-26T19:19:00Z"),
        [metaField]: "meta",
    };

    const closedBucketDoc = {
        "_id": ObjectId("63091c2c050b7495eaef4582"),
        "control": {
            "version": 1,
            "min": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:30Z")
            },
            "closed": true
        },
        "meta": "NonSuitableBucket1",
        "data": {
            "_id": {"0": ObjectId("63091c30138e9261fd70a903")},
            "time": {"0": ISODate("2022-08-26T19:19:30Z")}
        }
    };
    const compressedBucketDoc = {
        "_id": ObjectId("63091c2c050b7495eaef4583"),
        "control": {
            "version": 2,
            "min": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:30Z")
            },
            "closed": false
        },
        "meta": "NonSuitableBucket2",
        "data": {
            "_id": {"0": ObjectId("63091c30138e9261fd70a903")},
            "time": {"0": ISODate("2022-08-26T19:19:30Z")}
        }
    };
    const closedAndCompressedBucketDoc = {
        "_id": ObjectId("63091c2c050b7495eaef4584"),
        "control": {
            "version": 2,
            "min": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:30Z")
            },
            "closed": true
        },
        "meta": "NonSuitableBucket3",
        "data": {
            "_id": {"0": ObjectId("63091c30138e9261fd70a903")},
            "time": {"0": ISODate("2022-08-26T19:19:30Z")}
        }
    };
    const year2000BucketDoc = {
        "_id": ObjectId("63091c2c050b7495eaef4585"),
        "control": {
            "version": 1,
            "min": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2000-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2000-08-26T19:19:30Z")
            },
            "closed": false
        },
        "meta": "NonSuitableBucket4",
        "data": {
            "_id": {"0": ObjectId("63091c30138e9261fd70a903")},
            "time": {"0": ISODate("2022-08-26T19:19:30Z")}
        }
    };
    const metaMismatchFieldBucketDoc = {
        "_id": ObjectId("63091c2c050b7495eaef4586"),
        "control": {
            "version": 1,
            "min": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("63091c30138e9261fd70a903"),
                "time": ISODate("2022-08-26T19:19:30Z")
            },
            "closed": false
        },
        "meta": "metaMismatch",
        "data": {
            "_id": {"0": ObjectId("63091c30138e9261fd70a903")},
            "time": {"0": ISODate("2022-08-26T19:19:30Z")}
        }
    };

    assert.commandWorked(bucketsColl.insert(closedBucketDoc));
    // If an otherwise suitable bucket has the closed flag set, we expect to open a new bucket.
    checkIfBucketReopened(measurement1, /* willCreateBucket */ true, /* willReopenBucket */ false);

    assert.commandWorked(bucketsColl.insert(compressedBucketDoc));
    // If an otherwise suitable bucket is compressed, we expect to open a new bucket.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ true, /* willReopenBucket */ false);

    assert.commandWorked(bucketsColl.insert(closedAndCompressedBucketDoc));
    // If an otherwise suitable bucket is compressed and closed, we expect to open a new bucket.
    checkIfBucketReopened(measurement3, /* willCreateBucket */ true, /* willReopenBucket */ false);

    assert.commandWorked(bucketsColl.insert(year2000BucketDoc));
    // If an otherwise suitable bucket has an incompatible time range with the measurement, we
    // expect to open a new bucket.
    checkIfBucketReopened(measurement4, /* willCreateBucket */ true, /* willReopenBucket */ false);

    assert.commandWorked(bucketsColl.insert(metaMismatchFieldBucketDoc));
    // If an otherwise suitable bucket has a mismatching meta field, we expect to open a new bucket.
    checkIfBucketReopened(measurement5, /* willCreateBucket */ true, /* willReopenBucket */ false);

    jsTestLog("Exiting failToReopenNonSuitableBuckets.");
}();
})();
