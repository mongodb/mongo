/**
 * Inserts time-series measurements into closed buckets identified by query-based reopening method.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   tenant_migration_incompatible,
 *   # This test inserts uncompressed buckets directly into the buckets collection. This may cause
 *   # intermittent failures on tenant migration passthroughs when validation checks that all
 *   # buckets are compressed.
 *   tenant_migration_incompatible,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # This test depends on stats read from the primary node in replica sets.
 *   assumes_read_preference_unchanged,
 *   # TODO SERVER-89764 a concurrent moveCollection during insertion can cause the bucket
 *   # collection to insert more documents then expected by the test.
 *   assumes_balancer_off,
 *   # On 8.0+ this test makes assertions around the fact that we can reopen compressed buckets,
 *   # whereas on 7.0 we cannot.
 *   requires_fcv_80,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.timeseries_reopened_bucket_insert;
const bucketsColl = testDB["system.buckets." + coll.getName()];
const timeField = "time";
const metaField = "mm";
const metaTimeIndexName = [[metaField], "1", [timeField], "1"].join("_");

const resetCollection = function() {
    coll.drop();
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));
};

const checkIfBucketReopened = function(
    measurement, willCreateBucket = false, willReopenBucket = false) {
    let stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    const prevBucketCount = TimeseriesTest.getStat(stats.timeseries, 'bucketCount');
    const prevExpectedReopenedBuckets =
        TimeseriesTest.getStat(stats.timeseries, 'numBucketsReopened');

    const expectedReopenedBuckets =
        (willReopenBucket) ? prevExpectedReopenedBuckets + 1 : prevExpectedReopenedBuckets;
    const expectedBucketCount = (willCreateBucket) ? prevBucketCount + 1 : prevBucketCount;
    assert.commandWorked(coll.insert(measurement));

    stats = assert.commandWorked(coll.stats());
    assert(stats.timeseries);
    assert.eq(stats.timeseries['bucketCount'], expectedBucketCount);
    assert.eq(TimeseriesTest.getStat(stats.timeseries, 'numBucketsReopened'),
              expectedReopenedBuckets);
};

(function expectNoBucketReopening() {
    jsTestLog("Entering expectNoBucketReopening...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Bucket1",
    };
    const measurement2 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Bucket1",
    };

    // When there are no open buckets available and none to reopen, we expect to create a new one.
    checkIfBucketReopened(measurement1, /* willCreateBucket */ true, /* willReopenBucket */ false);
    // We don't expect buckets to be created or reopened when a suitable, open bucket exists.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ false, /* willReopenBucket */ false);

    jsTestLog("Exiting expectNoBucketReopening.");
})();

(function expectToReopenBuckets() {
    jsTestLog("Entering expectToReopenBuckets...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "ReopenedBucket1",
    };
    const measurement2 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "ReopenedBucket1",
    };
    const measurement3 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "ReopenedBucket2",
    };

    const bucketDoc = {
        "_id": ObjectId("66ffbfe83d997f237a6b2009"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "ReopenedBucket1",
        "data": {"_id": BinData(7, "BwBm//kKJfUSHr8j28sA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const missingClosedFlagBucketDoc = {
        "_id": ObjectId("66ffbfe83d997f237a6b200b"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
        },
        "meta": "ReopenedBucket2",
        "data": {"_id": BinData(7, "BwBnAAErFWHgv3LTiA8A"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
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
})();

(function expectToReopenBucketsWithComplexMeta() {
    jsTestLog("Entering expectToReopenBucketsWithComplexMeta...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: {b: 1, a: 1},
    };
    const measurement2 = {[timeField]: ISODate("2024-10-04T10:14:00Z"), [metaField]: {b: 2, a: 2}};

    const bucketDoc = {
        "_id": ObjectId("66ffbfe83d997f237a6b2009"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": {a: 1, b: 1},
        "data": {"_id": BinData(7, "BwBm//kKJfUSHr8j28sA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };

    // Insert closed bucket into the system.buckets collection.
    assert.commandWorked(bucketsColl.insert(bucketDoc));

    // Can reopen bucket with complex metadata, even if field order in measurement is different.
    checkIfBucketReopened(measurement1, /* willCreateBucket */ false, /* willReopenBucket */ true);
    // Does not reopen bucket with different complex metadata value.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ true, /* willReopenBucket */ false);

    jsTestLog("Exiting expectToReopenBucketsWithComplexMeta.");
})();

(function expectToReopenArchivedBuckets() {
    jsTestLog("Entering expectToReopenArchivedBuckets...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Meta1",
    };
    const measurement2 = {
        [timeField]: ISODate("2024-12-04T10:14:00Z"),
        [metaField]: "Meta1",
    };
    const measurement3 = {
        [timeField]: ISODate("2024-10-04T10:15:00Z"),
        [metaField]: "Meta1",
    };

    checkIfBucketReopened(measurement1, /* willCreateBucket */ true, /* willReopenBucket */ false);
    // Archive the original bucket due to time forward.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ true, /* willReopenBucket */ false);
    // Reopen original bucket.
    checkIfBucketReopened(measurement3, /* willCreateBucket */ false, /* willReopenBucket */ true);

    jsTestLog("Exiting expectToReopenArchivedBuckets.");
})();

(function expectToReopenCompressedBuckets() {
    if (!TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(db)) {
        return;
    }

    jsTestLog("Entering expectToReopenCompressedBuckets...");
    resetCollection();

    let initialMeasurements = [];
    const timestamp = ISODate("2024-10-04T10:14:00Z");
    for (let i = 0; i < 5; ++i) {
        initialMeasurements.push({
            [timeField]: timestamp,
            [metaField]: "ReopenedBucket1",
        });
    }
    const forward = {
        [timeField]: ISODate("2024-10-05T10:14:00Z"),
        [metaField]: "ReopenedBucket1",
    };
    const backward = {
        [timeField]: timestamp,
        [metaField]: "ReopenedBucket1",
    };

    for (let i = 0; i < initialMeasurements.length; ++i) {
        checkIfBucketReopened(
            initialMeasurements[i], /* willCreateBucket= */ i == 0, /* willReopenBucket= */ false);
    }
    // Time forwards will open a new bucket, and close and compress the old one.
    checkIfBucketReopened(forward, /* willCreateBucket */ true, /* willReopenBucket */ false);
    assert.eq(2,
              bucketsColl.find({"control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
                  .toArray()
                  .length);

    // We expect to reopen the compressed bucket with time backwards.
    checkIfBucketReopened(backward, /* willCreateBucket= */ false, /* willReopenBucket= */ true);

    jsTestLog("Exiting expectToReopenCompressedBuckets.");
})();

(function failToReopenNonSuitableBuckets() {
    jsTestLog("Entering failToReopenNonSuitableBuckets...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "NonSuitableBucket1",
    };
    const measurement2 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "NonSuitableBucket2",
    };
    const measurement3 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Meta",
    };

    const closedBucketDoc = {
        "_id": ObjectId("66ffbfe83d997f237a6b2009"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": true
        },
        "meta": "NonSuitableBucket1",
        "data": {"_id": BinData(7, "BwBm//kKJfUSHr8j28sA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const year2000BucketDoc = {
        "_id": ObjectId("39a818243d997f237a6b200a"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66ffffe825f5121ebf23dbcd"),
                "time": ISODate("2000-08-26T19:19:00Z")
            },
            "max": {
                "_id": ObjectId("66ffffe825f5121ebf23dbcd"),
                "time": ISODate("2000-08-26T19:19:30Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "NonSuitableBucket2",
        "data": {"_id": BinData(7, "BwBm///oJfUSHr8j280A"), "time": BinData(7, "CQDQwZ444QAAAAA=")}
    };
    const metaMismatchFieldBucketDoc = {
        "_id": ObjectId("66ffbfe83d997f237a6b200b"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "MetaMismatch",
        "data": {"_id": BinData(7, "BwBm//kKJfUSHr8j28sA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };

    assert.commandWorked(bucketsColl.insert(closedBucketDoc));
    // If an otherwise suitable bucket has the closed flag set, we expect to open a new bucket.
    checkIfBucketReopened(measurement1, /* willCreateBucket */ true, /* willReopenBucket */ false);

    assert.commandWorked(bucketsColl.insert(year2000BucketDoc));
    // If an otherwise suitable bucket has an incompatible time range with the measurement, we
    // expect to open a new bucket.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ true, /* willReopenBucket */ false);

    assert.commandWorked(bucketsColl.insert(metaMismatchFieldBucketDoc));
    // If an otherwise suitable bucket has a mismatching meta field, we expect to open a new bucket.
    checkIfBucketReopened(measurement3, /* willCreateBucket */ true, /* willReopenBucket */ false);

    jsTestLog("Exiting failToReopenNonSuitableBuckets.");
})();

(function failToReopenBucketWithNoMetaTimeIndex() {
    jsTestLog("Entering failToReopenBucketWithNoMetaTimeIndex...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Suitable1",
    };
    const measurement2 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Suitable2",
    };
    const measurement3 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Suitable3",
    };

    const closedBucketDoc1 = {
        "_id": ObjectId("66ffbfe83d997f237a6b2009"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "Suitable1",
        "data": {"_id": BinData(7, "BwBm//kKJfUSHr8j28sA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const closedBucketDoc2 = {
        "_id": ObjectId("66ffbfe83d997f237a6b200b"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "Suitable2",
        "data": {"_id": BinData(7, "BwBnAAErFWHgv3LTiA8A"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const closedBucketDoc3 = {
        "_id": ObjectId("66ffbfe83d997f237a6b200c"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("6700012d1561e0bf72d38811"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("6700012d1561e0bf72d38811"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "Suitable3",
        "data": {"_id": BinData(7, "BwBnAAEtFWHgv3LTiBEA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };

    let metaTimeIndex = coll.getIndexes().filter(function(index) {
        return index.name == metaTimeIndexName;
    });
    assert(metaTimeIndex.length == 1);

    assert.commandWorked(bucketsColl.insert(closedBucketDoc1));
    // We expect to reopen the suitable bucket when inserting the first measurement.
    checkIfBucketReopened(measurement1, /* willCreateBucket */ false, /* willReopenBucket */ true);

    // Drop the meta time index.
    assert.commandWorked(coll.dropIndexes([metaTimeIndexName]));
    metaTimeIndex = coll.getIndexes().filter(function(index) {
        return index.name == metaTimeIndexName;
    });
    assert(metaTimeIndex.length == 0);

    assert.commandWorked(bucketsColl.insert(closedBucketDoc2));
    // We have a suitable bucket for the second measurement but it is only visible via query-based
    // reopening which is not supported without the meta and time index.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ true, /* willReopenBucket */ false);

    // Create a meta and time index for query-based reopening.
    assert.commandWorked(
        coll.createIndex({[metaField]: 1, [timeField]: 1}, {name: "generic_meta_time_index_name"}));

    assert.commandWorked(bucketsColl.insert(closedBucketDoc3));
    // Creating an index on meta and time will re-enable us to perform query-based reopening to
    // insert measurement 3 into a suitable bucket.
    checkIfBucketReopened(measurement3, /* willCreateBucket */ false, /* willReopenBucket */ true);

    jsTestLog("Exiting failToReopenBucketWithNoMetaTimeIndex.");
})();

(function reopenBucketsWhenSuitableIndexExists() {
    jsTestLog("Entering reopenBucketsWhenSuitableIndexExists...");
    resetCollection();

    const measurement1 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Suitable1",
    };
    const measurement2 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Suitable2",
    };
    const measurement3 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Suitable3",
    };
    const measurement4 = {
        [timeField]: ISODate("2024-10-04T10:14:00Z"),
        [metaField]: "Suitable4",
    };

    const closedBucketDoc1 = {
        "_id": ObjectId("66ffbfe83d997f237a6b2009"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66ffffe825f5121ebf23dbcd"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("66ffffe825f5121ebf23dbcd"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "Suitable1",
        "data": {"_id": BinData(7, "BwBm///oJfUSHr8j280A"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const closedBucketDoc2 = {
        "_id": ObjectId("66ffbfe83d997f237a6b200b"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("66fff90a25f5121ebf23dbcb"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "Suitable2",
        "data": {"_id": BinData(7, "BwBm//kKJfUSHr8j28sA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const closedBucketDoc3 = {
        "_id": ObjectId("66ffbfe83d997f237a6b200c"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false,

        },
        "meta": "Suitable3",
        "data": {"_id": BinData(7, "BwBnAAErFWHgv3LTiA8A"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const closedBucketDoc4 = {
        "_id": ObjectId("66ffbfe83d997f237a6b200d"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("6700012d1561e0bf72d38811"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("6700012d1561e0bf72d38811"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "meta": "Suitable4",
        "data": {"_id": BinData(7, "BwBnAAEtFWHgv3LTiBEA"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };

    // Drop the meta time index.
    assert.commandWorked(coll.dropIndexes([metaTimeIndexName]));
    let metaTimeIndex = coll.getIndexes().filter(function(index) {
        return index.name == metaTimeIndexName;
    });
    assert(metaTimeIndex.length == 0);

    // Create a partial index on meta and time.
    assert.commandWorked(
        coll.createIndex({[metaField]: 1, [timeField]: 1},
                         {name: "partialMetaTimeIndex", partialFilterExpression: {b: {$lt: 12}}}));

    assert.commandWorked(bucketsColl.insert(closedBucketDoc1));
    // We expect no buckets to be reopened because a partial index on meta and time cannot be used
    // for query based reopening.
    checkIfBucketReopened(measurement1, /* willCreateBucket */ true, /* willReopenBucket */ false);

    // Create an index on an arbitrary field.
    assert.commandWorked(coll.createIndex({"a": 1}, {name: "arbitraryIndex"}));

    assert.commandWorked(bucketsColl.insert(closedBucketDoc2));
    // We expect no buckets to be reopened because the index created cannot be used for query-based
    // reopening.
    checkIfBucketReopened(measurement2, /* willCreateBucket */ true, /* willReopenBucket */ false);

    // Create an index on an arbitrary field in addition to the meta and time fields.
    assert.commandWorked(
        coll.createIndex({"a": 1, [metaField]: 1, [timeField]: 1}, {name: "nonSuitableIndex"}));

    assert.commandWorked(bucketsColl.insert(closedBucketDoc3));
    // We expect no buckets to be reopened because the index created cannot be used for
    // query-based reopening.
    checkIfBucketReopened(measurement3, /* willCreateBucket */ true, /* willReopenBucket */ false);

    // Create a meta and time index with an additional key on another arbitrary, data field.
    assert.commandWorked(
        coll.createIndex({[metaField]: 1, [timeField]: 1, "a": 1}, {name: metaTimeIndexName}));

    assert.commandWorked(bucketsColl.insert(closedBucketDoc4));
    // We expect to be able to reopen the suitable bucket when inserting the measurement because as
    // long as an index covers the meta and time field, it can have additional keys.
    checkIfBucketReopened(measurement4, /* willCreateBucket */ false, /* willReopenBucket */ true);

    jsTestLog("Exiting reopenBucketsWhenSuitableIndexExists.");
})();

(function reopenBucketsWhenSuitableIndexExistsNoMeta() {
    jsTestLog("Entering reopenBucketsWhenSuitableIndexExistsNoMeta...");
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeField}}));

    const measurement1 = {[timeField]: ISODate("2024-11-04T10:14:00Z")};
    const measurement2 = {[timeField]: ISODate("2024-10-04T10:14:00Z")};
    const measurement3 = {[timeField]: ISODate("2024-09-04T10:14:00Z")};

    const closedBucketDoc1 = {
        "_id": ObjectId("67289e68b6fd311bf10f7f5e"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("67000a6d0d365e9edde2f6d5"),
                "time": ISODate("2024-11-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("67000a6d0d365e9edde2f6d5"),
                "time": ISODate("2024-11-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "data": {"_id": BinData(7, "BwBnAAptDTZent3i9tUA"), "time": BinData(7, "CQBAxqr2kgEAAAA=")}
    };
    const closedBucketDoc2 = {
        "_id": ObjectId("66ffbfe83d997f237a6b200b"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("6700012b1561e0bf72d3880f"),
                "time": ISODate("2024-10-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "data": {"_id": BinData(7, "BwBnAAErFWHgv3LTiA8A"), "time": BinData(7, "CQBAogVXkgEAAAA=")}
    };
    const closedBucketDoc3 = {
        "_id": ObjectId("66d832e8b6fd311bf10f7f5d"),
        "control": {
            "version": TimeseriesTest.BucketVersion.kCompressedSorted,
            "min": {
                "_id": ObjectId("670008af0d365e9edde2f6d3"),
                "time": ISODate("2024-09-04T10:14:00Z")
            },
            "max": {
                "_id": ObjectId("670008af0d365e9edde2f6d3"),
                "time": ISODate("2024-09-04T10:14:00Z")
            },
            "count": 1,
            "closed": false
        },
        "data": {"_id": BinData(7, "BwBnAAivDTZent3i9tMA"), "time": BinData(7, "CQBA2oa8kQEAAAA=")}
    };

    // Since the collection was created without a meta field, the index on meta and time shouldn't
    // exist.
    let metaTimeIndex = coll.getIndexes().filter(function(index) {
        return index.name == metaTimeIndexName;
    });
    assert(metaTimeIndex.length == 0);

    // If the collection is sharded, there will be an index on control.min.time that can satisfy the
    // reopening query, otherwise we can do some further tests.
    if (!FixtureHelpers.isSharded(bucketsColl)) {
        // Create a partial index on time.
        assert.commandWorked(coll.createIndex(
            {[timeField]: 1}, {name: "partialTimeIndex", partialFilterExpression: {b: {$lt: 12}}}));

        assert.commandWorked(bucketsColl.insert(closedBucketDoc1));
        // We expect no buckets to be reopened because a partial index on time cannot be used  for
        // query based reopening.
        checkIfBucketReopened(
            measurement1, /* willCreateBucket */ true, /* willReopenBucket */ false);

        // Create an index on an arbitrary field.
        assert.commandWorked(coll.createIndex({"a": 1, [timeField]: 1}, {name: "arbitraryIndex"}));

        assert.commandWorked(bucketsColl.insert(closedBucketDoc2));
        // We expect no buckets to be reopened because the index created cannot be used for
        // query-based reopening.
        checkIfBucketReopened(
            measurement2, /* willCreateBucket */ true, /* willReopenBucket */ false);

        // Create an index on time.
        assert.commandWorked(coll.createIndex({[timeField]: 1}, {name: "timeIndex"}));

        assert.commandWorked(bucketsColl.insert(closedBucketDoc3));
        // We expect to be able to reopen the suitable bucket when inserting the measurement because
        // as long as an index covers time field (when the collection has no metaField), it can have
        // additional keys.
        checkIfBucketReopened(
            measurement3, /* willCreateBucket */ false, /* willReopenBucket */ true);
    }

    jsTestLog("Exiting reopenBucketsWhenSuitableIndexExistsNoMeta.");
})();

coll.drop();
