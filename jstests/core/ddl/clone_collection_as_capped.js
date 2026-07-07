/**
 * Test cloneCollectionAsCapped against timeseries collections.
 *
 * @tags: [
 *  # The test runs commands that are not allowed with security token: cloneCollectionAsCapped.
 *  not_allowed_with_signed_security_token,
 *  requires_non_retryable_commands,
 *  requires_capped,
 *  # Sharded collections can't be capped.
 *  assumes_unsharded_collection,
 *  # cloneCollectionAsCapped command is not supported on mongos
 *  assumes_against_mongod_not_mongos,
 * ]
 */

// Check that we cannot clone a timeseries collection as capped, whether addressed by its view name
// or by its underlying system.buckets.* namespace.

const timeseriesCollName = "timeseriesColl";
db.getCollection(timeseriesCollName).drop();

assert.commandWorked(
    db.runCommand({create: timeseriesCollName, timeseries: {timeField: "time", metaField: "meta"}}));

const timeseriesColl = db.getCollection(timeseriesCollName);
assert(!timeseriesColl.isCapped());

assert.commandFailedWithCode(
    db.runCommand({
        cloneCollectionAsCapped: timeseriesColl.getName(),
        toCollection: "timeseriesCappedFromView",
        size: 1000
    }),
    ErrorCodes.CommandNotSupportedOnView);

const bucketsCollName = "system.buckets." + timeseriesCollName;
assert.commandFailedWithCode(
    db.runCommand({
        cloneCollectionAsCapped: bucketsCollName,
        toCollection: "timeseriesCappedFromBuckets",
        size: 1000
    }),
    ErrorCodes.IllegalOperation);

const collNames = db.getCollectionNames();
assert(!collNames.includes("timeseriesCappedFromView"));
assert(!collNames.includes("timeseriesCappedFromBuckets"));
