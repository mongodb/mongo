/**
 * Tests that time-series buckets collections can be created with clusteredIndex options directly,
 * independent of the time-series collection creation command. This supports tools that clone
 * collections using the output of listCollections, which includes the clusteredIndex option.
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_wiredtiger,
 *     sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
const tsColl = testDB.clustered_index_options;
const tsCollName = tsColl.getName();
const bucketsCollName = 'system.buckets.' + tsCollName;

assert.commandWorked(testDB.createCollection(bucketsCollName, {clusteredIndex: {}}));
assert.commandWorked(testDB.dropDatabase());

assert.commandWorked(testDB.createCollection(bucketsCollName, {clusteredIndex: {}}));
assert.commandWorked(testDB.dropDatabase());

assert.commandWorked(
    testDB.createCollection(bucketsCollName, {clusteredIndex: {expireAfterSeconds: 10}}));
assert.commandWorked(testDB.dropDatabase());

// Round-trip creating a time-series collection.  Use the output of listCollections to re-create
// the buckets collection.
assert.commandWorked(
    testDB.createCollection(tsCollName, {timeseries: {timeField: 'time', expireAfterSeconds: 10}}));

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

assert.commandFailedWithCode(testDB.createCollection(bucketsCollName, {clusteredIndex: {bad: 1}}),
                             40415);
assert.commandFailedWithCode(
    testDB.createCollection(bucketsCollName,
                            {clusteredIndex: {}, idIndex: {key: {_id: 1}, name: '_id_'}}),
    ErrorCodes.InvalidOptions);

// Using the 'clusteredIndex' option on any namespace other than a buckets namespace should fail.
assert.commandFailedWithCode(testDB.createCollection(tsCollName, {clusteredIndex: {}}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(testDB.createCollection('test', {clusteredIndex: {}}),
                             ErrorCodes.InvalidOptions);
})();