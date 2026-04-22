/**
 * Verifies that drop and collmod commands directly targeting system.buckets.* namespaces are rejected with
 * CommandNotSupportedOnLegacyTimeseriesBucketsNamespace when viewless timeseries are enabled.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

const collName = jsTestName();
const bucketsCollName = "system.buckets." + collName;

db.getCollection(collName).drop();
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "time", granularity: "seconds"}}));

// drop on system.buckets.* is rejected.
assert.commandFailedWithCode(
    db.runCommand({drop: bucketsCollName}),
    ErrorCodes.CommandNotSupportedOnLegacyTimeseriesBucketsNamespace,
);

// collMod on system.buckets.* is rejected.
assert.commandFailedWithCode(
    db.runCommand({"collMod": bucketsCollName, "timeseries": {"granularity": "hours"}}),
    ErrorCodes.CommandNotSupportedOnLegacyTimeseriesBucketsNamespace,
);
