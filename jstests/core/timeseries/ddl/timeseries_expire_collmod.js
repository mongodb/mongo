/**
 * Tests that collMod can change the expireAfterSeconds option on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const coll = db[jsTestName()];
coll.drop();

const timeFieldName = 'time';
const expireAfterSeconds = NumberLong(5);
assert.commandWorked(db.createCollection(
    coll.getName(),
    {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSeconds}));

// Cannot use the 'expireAfterSeconds' option on collections that aren't clustered.
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

let collInfo = getTimeseriesCollForDDLOps(db, coll).getMetadata();
assert.eq(expireAfterSeconds,
          collInfo.options.expireAfterSeconds,
          getTimeseriesCollForDDLOps(db, coll).getName() + ': ' + expireAfterSeconds + ': ' +
              tojson(collInfo));

/**
 * Runs collMod on 'collToChange' with the given 'expireAfterSeconds' value and checks the expected
 * value using listCollections on the collection.
 */
const runTest = function(collToChange, expireAfterSeconds) {
    assert.commandWorked(db.runCommand({
        collMod: collToChange.getName(),
        expireAfterSeconds: expireAfterSeconds,
    }));

    collInfo = getTimeseriesCollForDDLOps(db, coll).getMetadata();
    if (expireAfterSeconds !== 'off') {
        assert.eq(expireAfterSeconds,
                  collInfo.options.expireAfterSeconds,
                  collToChange.getFullName() + ': ' + expireAfterSeconds + ': ' + tojson(collInfo));
    } else {
        assert(!collInfo.options.hasOwnProperty("expireAfterSeconds"),
               collToChange.getFullName() + ': ' + expireAfterSeconds + ': ' + tojson(collInfo));
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