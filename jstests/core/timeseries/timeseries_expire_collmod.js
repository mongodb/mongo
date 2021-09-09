/**
 * Tests that collMod can change the expireAfterSeconds option on both a time-series collection and
 * the underlying bucket collection.
 *
 * @tags: [
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const coll = db.timeseries_expire_collmod;
coll.drop();

const timeFieldName = 'time';
const expireAfterSeconds = NumberLong(5);
assert.commandWorked(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSeconds}));

const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

// Cannot use the 'clusteredIndex' option on collections that aren't time-series bucket collections.
const collNotClustered = db.getCollection(coll.getName() + '_not_clustered');
collNotClustered.drop();
assert.commandWorked(db.createCollection(collNotClustered.getName()));
assert.commandFailedWithCode(
    db.runCommand({collMod: collNotClustered.getName(), expireAfterSeconds: 10}),
    ErrorCodes.InvalidOptions);

// Check for invalid input on the time-series collection.
assert.commandFailedWithCode(db.runCommand({collMod: coll.getName(), expireAfterSeconds: "10"}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({collMod: coll.getName(), expireAfterSeconds: {}}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(db.runCommand({collMod: coll.getName(), expireAfterSeconds: -10}),
                             ErrorCodes.InvalidOptions);

// Check for invalid input on the underlying bucket collection.
assert.commandFailedWithCode(
    db.runCommand({collMod: bucketsColl.getName(), expireAfterSeconds: "10"}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    db.runCommand({collMod: bucketsColl.getName(), expireAfterSeconds: {}}),
    ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(
    db.runCommand({collMod: bucketsColl.getName(), expireAfterSeconds: -10}),
    ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({
    collMod: bucketsColl.getName(),
    expireAfterSeconds: NumberLong("4611686018427387904"),
}),
                             ErrorCodes.InvalidOptions);

let res = assert.commandWorked(
    db.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
assert.eq(expireAfterSeconds,
          res.cursor.firstBatch[0].options.expireAfterSeconds,
          bucketsColl.getName() + ': ' + expireAfterSeconds + ': ' + tojson(res));

/**
 * Runs collMod on 'collToChange' with the given 'expireAfterSeconds' value and checks the expected
 * value using listCollections on the bucketCollection.
 */
const runTest = function(collToChange, expireAfterSeconds) {
    assert.commandWorked(db.runCommand({
        collMod: collToChange.getName(),
        expireAfterSeconds: expireAfterSeconds,
    }));

    res = assert.commandWorked(
        db.runCommand({listCollections: 1, filter: {name: bucketsColl.getName()}}));
    if (expireAfterSeconds !== 'off') {
        assert.eq(expireAfterSeconds,
                  res.cursor.firstBatch[0].options.expireAfterSeconds,
                  collToChange.getFullName() + ': ' + expireAfterSeconds + ': ' + tojson(res));
    } else {
        assert(!res.cursor.firstBatch[0].options.hasOwnProperty("expireAfterSeconds"),
               collToChange.getFullName() + ': ' + expireAfterSeconds + ': ' + tojson(res));
    }
};

// Tests for collMod on the time-series collection.

// Change expireAfterSeconds to 10.
runTest(coll, 10);

// Change expireAfterSeconds to 0.
runTest(coll, 0);

// Disable expireAfterSeconds.
runTest(coll, 'off');

// Enable expireAfterSeconds again.
runTest(coll, 100);

// Tests for collMod on the underlying bucket collection.

// Change expireAfterSeconds to 10.
runTest(bucketsColl, 10);

// Change expireAfterSeconds to 0.
runTest(bucketsColl, 0);

// Disable expireAfterSeconds.
runTest(bucketsColl, 'off');

// Enable expireAfterSeconds again.
runTest(bucketsColl, 100);
})();
