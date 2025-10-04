/**
 * This tests which collMod options are allowed on a time-series collection.
 *
 * @tags: [
 *   # Behavior clarified in binVersion 6.1
 *   requires_fcv_61,
 *   # collMod is not retryable
 *   requires_non_retryable_commands,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const collName = jsTestName();
const bucketsCollName = "system.buckets." + collName;
const coll = db.getCollection(collName);
const bucketMaxSpanSecondsHours = 60 * 60 * 24 * 30;
const bucketRoundingSecondsHours = 60 * 60 * 24;
const bucketingValueMax = 86400 * 365; // Seconds in a year.
const idlInvalidValueError = 51024;

coll.drop();
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "time", granularity: "seconds"}}));

// This test cannot use the index with the key {'time': 1}, since that is the same as the implicitly
// created shard key and thus we cannot hide the index. We will use a different index here to avoid
// conflicts.
const indexField = "a";
assert.commandWorked(coll.createIndex({[indexField]: 1}));

// Tries to convert a time-series secondary index to TTL index.
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "index": {"keyPattern": {[indexField]: 1}, "expireAfterSeconds": 100},
    }),
    ErrorCodes.InvalidOptions,
);

// Successfully hides a time-series secondary index.
assert.commandWorked(db.runCommand({"collMod": collName, "index": {"keyPattern": {[indexField]: 1}, "hidden": true}}));

// Hiding the internal clustered index fails, since the index is not exposed to users.
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, index: {name: "_id_", hidden: true}}),
    ErrorCodes.IndexNotFound,
);

// Tries to set the validator for a time-series collection.
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "validator": {required: ["time"]}}),
    ErrorCodes.InvalidOptions,
);

if (!areViewlessTimeseriesEnabled(db)) {
    // Tries to set the validator for a time-series collection using the buckets namespace.
    assert.commandFailedWithCode(db.runCommand({"collMod": bucketsCollName, "validator": {required: ["time"]}}), [
        ErrorCodes.InvalidNamespace,
        // TODO SERVER-105548 Remove the following error code once 9.0 becomes LTS. Needed for
        // multiversion compatibility. As of 8.3 calling collMod on the buckets namespace
        // returns InvalidNamespace.
        ErrorCodes.InvalidOptions,
    ]);
}

// Tries to set the validationLevel for a time-series collection.
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "validationLevel": "moderate"}),
    ErrorCodes.InvalidOptions,
);

if (!areViewlessTimeseriesEnabled(db)) {
    // Tries to set the validationLevel for a time-series collection using the buckets namespace.
    assert.commandFailedWithCode(db.runCommand({"collMod": bucketsCollName, "validationLevel": "moderate"}), [
        ErrorCodes.InvalidNamespace,
        // TODO SERVER-105548 Remove the following error code once 9.0 becomes LTS. Needed for
        // multiversion compatibility. As of 8.3 calling collMod on the buckets namespace
        // returns InvalidNamespace.
        ErrorCodes.InvalidOptions,
    ]);
}

// Tries to set the validationAction for a time-series collection.
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "validationAction": "warn"}),
    ErrorCodes.InvalidOptions,
);

if (!areViewlessTimeseriesEnabled(db)) {
    // Tries to set the validationLevel for a time-series collection using the buckets namespace.
    assert.commandFailedWithCode(db.runCommand({"collMod": bucketsCollName, "validationAction": "warn"}), [
        ErrorCodes.InvalidNamespace,
        // TODO SERVER-105548 Remove the following error code once 9.0 becomes LTS. Needed for
        // multiversion compatibility. As of 8.3 calling collMod on the buckets namespace
        // returns InvalidNamespace.
        ErrorCodes.InvalidOptions,
    ]);
}

// Tries to modify the view for a time-series collection.
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "viewOn": "foo", "pipeline": []}),
    ErrorCodes.InvalidOptions,
);

// Successfully sets 'expireAfterSeconds' for a time-series collection.
assert.commandWorked(db.runCommand({"collMod": collName, "expireAfterSeconds": 60}));

// Successfully sets the granularity to a higher value for a time-series collection.
assert.commandWorked(db.runCommand({"collMod": collName, "timeseries": {"granularity": "minutes"}}));

// Tries to set current granularity to a lower value.
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "timeseries": {"granularity": "seconds"}}),
    ErrorCodes.InvalidOptions,
);

// Successfully sets the granularity to a higher value for a time-series collection.
assert.commandWorked(db.runCommand({"collMod": collName, "timeseries": {"granularity": "hours"}}));

// collMod against the underlying buckets collection should fail: not allowed to target the buckets
// collection.
assert.commandFailedWithCode(
    db.runCommand({"collMod": getTimeseriesBucketsColl(collName), "timeseries": {"granularity": "hours"}}),
    [
        ErrorCodes.InvalidNamespace,
        // Error code thrown by collmod coordinator.
        //
        // TODO SERVER-105548 remove the following error code once 9.0 becomes last LTS
        // Currently needded for multiversion compability. Since 8.2 we throw InvalidNamespace also
        // on sharded clusters.
        6201808,
    ],
);

// Tries to set one seconds parameter without the other (bucketMaxSpanSeconds or
// bucketRoundingSeconds).
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "timeseries": {"bucketMaxSpanSeconds": bucketMaxSpanSecondsHours}}),
    ErrorCodes.InvalidOptions,
);
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "timeseries": {"bucketRoundingSeconds": bucketRoundingSecondsHours}}),
    ErrorCodes.InvalidOptions,
);

// Tries to set bucketMaxSpanSeconds and bucketRoundingSeconds to a value less than the current
// maxSpanSeconds.
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "timeseries": {
            "bucketMaxSpanSeconds": bucketRoundingSecondsHours,
            "bucketRoundingSeconds": bucketRoundingSecondsHours,
        },
    }),
    ErrorCodes.InvalidOptions,
);

// Expect setting the bucketMaxSpanSeconds corresponding to the granularity default value to
// succeed.
assert.commandWorked(
    db.runCommand({
        "collMod": collName,
        "timeseries": {"granularity": "hours", "bucketMaxSpanSeconds": bucketMaxSpanSecondsHours},
    }),
);

// Tries to set bucketMaxSpanSeconds and bucketRoundingSeconds with different values
// corresponding to the granularity default values. This should fail since they are not equal.
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "timeseries": {
            "granularity": "hours",
            "bucketMaxSpanSeconds": bucketMaxSpanSecondsHours,
            "bucketRoundingSeconds": bucketRoundingSecondsHours,
        },
    }),
    ErrorCodes.InvalidOptions,
);

// Tries to set bucketMaxSpanSeconds and bucketRoundingSeconds with different values.
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "timeseries": {
            "bucketMaxSpanSeconds": bucketMaxSpanSecondsHours + 1,
            "bucketRoundingSeconds": bucketRoundingSecondsHours + 1,
        },
    }),
    ErrorCodes.InvalidOptions,
);

// Tries to set granularity, bucketMaxSpanSeconds and bucketRoundingSeconds with different
// values from their default (60 * 60 * 24 and 60 * 60).
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "timeseries": {
            "granularity": "hours",
            "bucketMaxSpanSeconds": bucketMaxSpanSecondsHours + 1,
            "bucketRoundingSeconds": bucketRoundingSecondsHours + 1,
        },
    }),
    ErrorCodes.InvalidOptions,
);

// Successfully sets bucketMaxSpanSeconds, bucketRoundingSeconds and granularity to an equal
// value. This accepts the 3 parameters because they are the same as the current set values.
assert.commandWorked(
    db.runCommand({
        "collMod": collName,
        "timeseries": {"granularity": "hours", "bucketMaxSpanSeconds": bucketMaxSpanSecondsHours},
    }),
);

// Successfully sets the bucketMaxSpanSeconds and bucketRoundingSeconds to a higher value for a
// timeseries collection. The granularity is currently set to 'hours' for this collection so we
// should be able to increase the value of bucketMaxSpanSeconds by one.
assert.commandWorked(
    db.runCommand({
        "collMod": collName,
        "timeseries": {
            "bucketMaxSpanSeconds": bucketMaxSpanSecondsHours + 1,
            "bucketRoundingSeconds": bucketMaxSpanSecondsHours + 1,
        },
    }),
);

// Setting prepareUnique should return an error on a time-series collection index.
assert.commandWorked(coll.createIndex({"prepareUniqueIndex": 1}));
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "index": {"keyPattern": {"prepareUniqueIndex": 1}, "prepareUnique": true},
    }),
    ErrorCodes.InvalidOptions,
);

assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "index": {"keyPattern": {"prepareUniqueIndex": 1}, "prepareUnique": false},
    }),
    ErrorCodes.InvalidOptions,
);

// Verify seconds was correctly set on the collection and granularity removed since a custom
// value was added.
if (!areViewlessTimeseriesEnabled(db)) {
    const collectionEntry = getTimeseriesBucketsColl(coll).getMetadata();
    assert(collectionEntry);

    assert.eq(collectionEntry.options.timeseries.bucketRoundingSeconds, bucketMaxSpanSecondsHours + 1);
    assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds, bucketMaxSpanSecondsHours + 1);
    assert.isnull(collectionEntry.options.timeseries.granularity);
}

const collectionEntry = coll.getMetadata();
assert(collectionEntry);
assert.eq(collectionEntry.options.timeseries.bucketRoundingSeconds, bucketMaxSpanSecondsHours + 1);
assert.eq(collectionEntry.options.timeseries.bucketMaxSpanSeconds, bucketMaxSpanSecondsHours + 1);
assert.isnull(collectionEntry.options.timeseries.granularity);

coll.drop();
// Create timeseries collection with custom maxSpanSeconds and bucketRoundingSeconds.
assert.commandWorked(
    db.createCollection(collName, {
        timeseries: {timeField: "time", bucketMaxSpanSeconds: 200, bucketRoundingSeconds: 200},
    }),
);

// Successfully sets granularity from a collection created with custom maxSpanSeconds and
// bucketRoundingSeconds since the default values for 'minutes' are greater than the previous
// seconds.
assert.commandWorked(db.runCommand({"collMod": collName, "timeseries": {"granularity": "minutes"}}));

// Fails to set bucketMaxSpanSeconds and bucketRoundingSeconds past the bucketing limit.
assert.commandFailedWithCode(
    db.runCommand({
        "collMod": collName,
        "timeseries": {
            "bucketMaxSpanSeconds": bucketingValueMax + 1,
            "bucketRoundingSeconds": bucketingValueMax + 1,
        },
    }),
    [ErrorCodes.BadValue, idlInvalidValueError],
); // getting BadValue when binary is > 7.1, else idlInvalidValueError

// Successfully set the bucketMaxSpanSeconds and bucketRoundingSeconds to the limit.
assert.commandWorked(
    db.runCommand({
        "collMod": collName,
        "timeseries": {"bucketMaxSpanSeconds": bucketingValueMax, "bucketRoundingSeconds": bucketingValueMax},
    }),
);

// No-op command should succeed with empty time-series options.
assert.commandWorked(db.runCommand({"collMod": collName, "timeseries": {}}));
