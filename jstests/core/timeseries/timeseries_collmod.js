/**
 * This tests which collMod options are allowed on a time-series collection.
 *
 * @tags: [
 *  # Cannot implicitly shard accessed collections because of collection existing when none
 *  # expected.
 *  assumes_no_implicit_collection_creation_after_drop,
 *  requires_non_retryable_commands,
 *  requires_fcv_61,
 * ]
 */

(function() {
'use strict';
const collName = "timeseries_collmod";
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: "time", granularity: 'seconds'}}));
assert.commandWorked(coll.createIndex({"time": 1}));

// Tries to convert a time-series secondary index to TTL index.
assert.commandFailedWithCode(
    db.runCommand(
        {"collMod": collName, "index": {"keyPattern": {"time": 1}, "expireAfterSeconds": 100}}),
    ErrorCodes.InvalidOptions);

// Successfully hides a time-series secondary index.
assert.commandWorked(
    db.runCommand({"collMod": collName, "index": {"keyPattern": {"time": 1}, "hidden": true}}));

// Tries to set the validator for a time-series collection.
assert.commandFailedWithCode(
    db.runCommand({"collMod": collName, "validator": {required: ["time"]}}),
    ErrorCodes.InvalidOptions);

// Tries to set the validationLevel for a time-series collection.
assert.commandFailedWithCode(db.runCommand({"collMod": collName, "validationLevel": "moderate"}),
                             ErrorCodes.InvalidOptions);

// Tries to set the validationAction for a time-series collection.
assert.commandFailedWithCode(db.runCommand({"collMod": collName, "validationAction": "warn"}),
                             ErrorCodes.InvalidOptions);

// Tries to modify the view for a time-series collection.
assert.commandFailedWithCode(db.runCommand({"collMod": collName, "viewOn": "foo", "pipeline": []}),
                             ErrorCodes.InvalidOptions);

// Tries to set 'recordPreImages' for a time-series collection.
assert.commandFailedWithCode(db.runCommand({"collMod": collName, "recordPreImages": true}),
                             ErrorCodes.InvalidOptions);

// Successfully sets 'expireAfterSeconds' for a time-series collection.
assert.commandWorked(db.runCommand({"collMod": collName, "expireAfterSeconds": 60}));

// Successfully sets the granularity for a time-series collection.
assert.commandWorked(
    db.runCommand({"collMod": collName, "timeseries": {"granularity": "minutes"}}));

// collMod against the underlying buckets collection should fail: not allowed to target the buckets
// collection.
assert.commandFailedWithCode(
    db.runCommand(
        {"collMod": ("system.buckets." + collName), "timeseries": {"granularity": "hours"}}),
    [ErrorCodes.InvalidNamespace, 6201808 /* mongos error code */]);
})();
