/**
 * Tests initial sync with a time-series collection that contains mixed-schema buckets.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

TestData.skipEnforceTimeseriesBucketsAreAlwaysCompressedOnValidate = true;

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const db = primary.getDB(jsTestName());
const coll = db.coll;
const bucketsColl = db["system.buckets." + coll.getName()];

const bucket = {
    _id: ObjectId("65a6eb806ffc9fa4280ecac4"),
    control: {
        version: NumberInt(1),
        min: {
            _id: ObjectId("65a6eba7e6d2e848e08c3750"),
            t: ISODate("2024-01-16T20:48:00Z"),
            a: 1,
        },
        max: {
            _id: ObjectId("65a6eba7e6d2e848e08c3751"),
            t: ISODate("2024-01-16T20:48:39.448Z"),
            a: "a",
        },
    },
    meta: 0,
    data: {
        _id: {
            0: ObjectId("65a6eba7e6d2e848e08c3750"),
            1: ObjectId("65a6eba7e6d2e848e08c3751"),
        },
        t: {
            0: ISODate("2024-01-16T20:48:39.448Z"),
            1: ISODate("2024-01-16T20:48:39.448Z"),
        },
        a: {
            0: "a",
            1: 1,
        },
    }
};

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
assert.commandWorked(
    db.runCommand({collMod: coll.getName(), timeseriesBucketsMayHaveMixedSchemaData: true}));
assert.commandWorked(bucketsColl.insert(bucket));

const secondary = replTest.add();
replTest.reInitiate();
replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);
replTest.awaitReplication();

const primaryColl = primary.getDB(db.getName())[bucketsColl.getName()];
const secondaryColl = secondary.getDB(db.getName())[bucketsColl.getName()];

assert(TimeseriesTest.bucketsMayHaveMixedSchemaData(primaryColl));
assert(TimeseriesTest.bucketsMayHaveMixedSchemaData(secondaryColl));

replTest.stopSet();
})();
