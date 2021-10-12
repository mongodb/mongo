/**
 * Tests that time-series buckets collections can be created with clusteredIndex options directly,
 * independent of the time-series collection creation command. This supports tools that clone
 * collections using the output of listCollections, which includes the clusteredIndex option.
 *
 * @tags: [
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
const tsColl = testDB.clustered_index_options;
const tsCollName = tsColl.getName();
const bucketsCollName = 'system.buckets.' + tsCollName;

assert.commandWorked(testDB.createCollection(bucketsCollName, {clusteredIndex: false}));
assert.commandWorked(testDB.dropDatabase());

assert.commandWorked(testDB.createCollection(bucketsCollName, {clusteredIndex: true}));
assert.commandWorked(testDB.dropDatabase());

assert.commandWorked(
    testDB.createCollection(bucketsCollName, {clusteredIndex: true, expireAfterSeconds: 10}));
assert.commandWorked(testDB.dropDatabase());

// Round-trip creating a time-series collection.  Use the output of listCollections to re-create
// the buckets collection.
assert.commandWorked(
    testDB.createCollection(tsCollName, {timeseries: {timeField: 'time'}, expireAfterSeconds: 10}));

let res =
    assert.commandWorked(testDB.runCommand({listCollections: 1, filter: {name: bucketsCollName}}));
let options = res.cursor.firstBatch[0].options;
assert(options.clusteredIndex);
assert(tsColl.drop());

assert.commandWorked(testDB.createCollection(bucketsCollName, options));
res =
    assert.commandWorked(testDB.runCommand({listCollections: 1, filter: {name: bucketsCollName}}));
assert.eq(options, res.cursor.firstBatch[0].options);
assert.commandWorked(testDB.dropDatabase());

assert.commandFailedWithCode(testDB.createCollection(bucketsCollName, {clusteredIndex: {}}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(testDB.createCollection(bucketsCollName, {clusteredIndex: 'a'}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(
    testDB.createCollection(bucketsCollName,
                            {clusteredIndex: true, idIndex: {key: {_id: 1}, name: '_id_'}}),
    ErrorCodes.InvalidOptions);

// Using the 'clusteredIndex' option on any namespace other than a buckets namespace should fail.
assert.commandFailedWithCode(testDB.createCollection(tsCollName, {clusteredIndex: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(testDB.createCollection('test', {clusteredIndex: true}),
                             ErrorCodes.InvalidOptions);

// Using the 'expireAfterSeconds' option on any namespace other than a time-series namespace or a
// clustered time-series buckets namespace should fail.
assert.commandFailedWithCode(testDB.createCollection('test', {expireAfterSeconds: 10}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(testDB.createCollection(bucketsCollName, {expireAfterSeconds: 10}),
                             ErrorCodes.InvalidOptions);
})();
