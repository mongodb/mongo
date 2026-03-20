/**
 * Test that timeseries collmod options can only be submitted without other collmod options.
 *
 * @tags: [
 *   # collMod is not retryable
 *   requires_non_retryable_commands,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

const coll = db["coll"];
const indexField = "a";
const bucketRoundingSecondsHours = 60 * 60 * 24;

function createTestColl() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(),
                            {timeseries: {timeField: "time", granularity: "seconds"}}),
    );

    // This test cannot use the index with the key {'time': 1}, since that is the same as the
    // implicitly created shard key and thus we cannot hide the index. We will use a different index
    // here to avoid conflicts.
    assert.commandWorked(coll.createIndex({[indexField]: 1}));
}

const timeseriesOptions = [
    {"timeseries": {"granularity": "minutes"}},
    {
        "timeseries": {
            "bucketMaxSpanSeconds": bucketRoundingSecondsHours,
            "bucketRoundingSeconds": bucketRoundingSecondsHours,
        },
    }
];
const nonTimeseriesValidOptions = [
    {"index": {"keyPattern": {[indexField]: 1}, "hidden": true}},
    {"expireAfterSeconds": 60},
    {"timeseriesBucketsMayHaveMixedSchemaData": true},
];
const nonTimeseriesInvalidOptions = [
    {"index": {"keyPattern": {[indexField]: 1}, "expireAfterSeconds": 100}},
    {"validator": {required: ["time"]}},
    {"validationLevel": "moderate"},
];

// Test that valid options alone works
for (const opt of [...timeseriesOptions, ...nonTimeseriesValidOptions]) {
    createTestColl();
    assert.commandWorked(db.runCommand({"collMod": coll.getName(), ...opt}));
}

createTestColl();
// Test that valid timeseries options combined with other options always return InvalidOptions error
for (const timeseriesOpt of timeseriesOptions) {
    for (const nonTimeseriesOpt of [...nonTimeseriesInvalidOptions, ...nonTimeseriesValidOptions]) {
        assert.commandFailedWithCode(
            db.runCommand({"collMod": coll.getName(), ...timeseriesOpt, ...nonTimeseriesOpt}),
            ErrorCodes.InvalidOptions,
        );
    }
}
