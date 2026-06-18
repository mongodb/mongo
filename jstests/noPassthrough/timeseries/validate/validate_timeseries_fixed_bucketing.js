/**
 * Tests that the validate command checks data consistencies of the fixedBucketing field.
 *
 * Three scenarios, each tested for two bucketing configurations:
 *
 * 1. Feature flag off OR fixedBucketing: false — validate passes regardless of bucket alignment.
 * 2. Feature flag on, default collection (fixedBucketing: true) — correct bucket passes, misaligned bucket fails.
 *
 * Raw bucket inserts are used throughout so the test does not depend on collMod or any failpoint
 * to create the inconsistent state.
 *
 * @tags: [
 * requires_fcv_90
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

// TODO(SERVER-127534): Remove once featureFlagFixedBucketingCatalog is on by default.
const fixedBucketingEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "FixedBucketingCatalog");

let testCount = 0;

function makeCollName() {
    return jsTestName() + "_" + testCount++;
}

// Returns an ObjectId whose embedded timestamp matches `date`.
function makeOidForTimestamp(date) {
    const secs = Math.floor(date.getTime() / 1000);
    return new ObjectId(secs.toString(16).padStart(8, "0") + "0000000000000000");
}

// Constructs a minimal v1 (uncompressed) bucket with a single measurement at `minDate`.
function makeBucketDoc(minDate) {
    return {
        _id: makeOidForTimestamp(minDate),
        control: {
            version: NumberInt(1),
            min: {timestamp: minDate},
            max: {timestamp: minDate},
            count: NumberInt(1),
        },
        meta: {sensorId: 1},
        data: {timestamp: {0: minDate}},
    };
}

function createColl(name, tsParams) {
    db.getCollection(name).drop();
    assert.commandWorked(db.createCollection(name, {timeseries: tsParams}));
    return db.getCollection(name);
}

function insertRawBucket(coll, bucket) {
    assert.commandWorked(
        getTimeseriesCollForRawOps(db, coll).insertOne(bucket, getRawOperationSpec(db)),
    );
}

// Runs the test scenarios for a given bucketing configuration.
// `consistentMin` rounds to itself under `bucketingParams`.
// `inconsistentMin` does not round to itself under `bucketingParams`.
function runScenarios(bucketingParams, consistentMin, inconsistentMin) {
    const baseParams = {timeField: "timestamp", metaField: "metadata", ...bucketingParams};

    // Helper function asserting that validate passes for both a consistent and an inconsistent bucket.
    // Used when fixedBucketing is off (flag absent or explicitly false), i.e., when no alignment check is performed.
    function assertBothPass(collParams) {
        const coll1 = createColl(makeCollName(), collParams);
        insertRawBucket(coll1, makeBucketDoc(consistentMin));
        assert(coll1.validate().valid, "consistent bucket failed validate unexpectedly");

        const coll2 = createColl(makeCollName(), collParams);
        insertRawBucket(coll2, makeBucketDoc(inconsistentMin));
        assert(coll2.validate().valid, "inconsistent bucket failed validate unexpectedly");
    }

    if (!fixedBucketingEnabled) {
        // Flag off: fixedBucketing is not stored, so validate never checks alignment.
        jsTest.log.info("Flag off — both buckets expected to pass.");
        assertBothPass(baseParams);
        return;
    }

    // Flag on, fixedBucketing: false — explicitly disabled, same outcome as flag off.
    jsTest.log.info("Flag on, fixedBucketing:false — both buckets expected to pass.");
    assertBothPass({...baseParams, fixedBucketing: false});

    // Flag on, default collection (fixedBucketing: true):
    // correct bucket passes, misaligned bucket fails.
    jsTest.log.info("Flag on, default collection: consistent bucket — expect valid.");
    {
        const coll = createColl(makeCollName(), baseParams);
        insertRawBucket(coll, makeBucketDoc(consistentMin));
        assert(coll.validate().valid, tojson(coll.validate()));
    }

    jsTest.log.info("Flag on, default collection: misaligned bucket — expect validate error.");
    {
        const coll = createColl(makeCollName(), baseParams);
        insertRawBucket(coll, makeBucketDoc(inconsistentMin));
        const res = coll.validate();
        assert(!res.valid, tojson(res));
        assert.contains(
            "A time series bucketing parameter was changed in this collection but fixedBucketing is true. " +
                "For more info, see logs with log id 9175400.",
            res.errors,
        );
    }
}

// Seconds granularity rounds to the minute boundary (60s).
// 13:25:00Z is consistent; 13:25:30Z rounds to 13:25:00Z so it is inconsistent.
jsTest.log.info("Testing fixedBucketing consistency check for granularity bucketing parameter.");
runScenarios(
    {granularity: "seconds"},
    ISODate("2024-04-01T13:25:00.000Z"),
    ISODate("2024-04-01T13:25:30.000Z"),
);

// 240s (4-minute) rounding. 13:24:00Z is consistent; 13:25:30Z rounds to 13:24:00Z.
jsTest.log.info(
    "Testing fixedBucketing consistency check for bucketRoundingSeconds and " +
        "bucketMaxSpanSeconds bucketing parameters.",
);
runScenarios(
    {bucketMaxSpanSeconds: 240, bucketRoundingSeconds: 240},
    ISODate("2024-04-01T13:24:00.000Z"),
    ISODate("2024-04-01T13:25:30.000Z"),
);

MongoRunner.stopMongod(conn, null, {skipValidation: true});
