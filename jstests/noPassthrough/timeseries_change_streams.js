// Tests that $changeStream aggregations against time-series collections fail cleanly.
// @tags: [
//  requires_timeseries,
//  requires_replication,
// ]
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");     // For FixtureHelpers.
load("jstests/libs/change_stream_util.js");  // For ChangeStreamTest and
                                             // assert[Valid|Invalid]ChangeStreamNss.

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const timeFieldName = "time";
const metaFieldName = "tags";
const testDB = rst.getPrimary().getDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const tsColl = testDB.getCollection("ts_point_data");
tsColl.drop();

assert.commandWorked(testDB.createCollection(
    tsColl.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

const nMeasurements = 10;

for (let i = 0; i < nMeasurements; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: i.toString(),
        value: i + nMeasurements,
    };
    assert.commandWorked(tsColl.insert(docToInsert));
}

// Test that a changeStream cannot be opened on 'system.buckets.X' collections.
assert.commandFailedWithCode(testDB.runCommand({
    aggregate: "system.buckets." + tsColl.getName(),
    pipeline: [{$changeStream: {}}],
    cursor: {}
}),
                             ErrorCodes.InvalidNamespace);

// Test that a changeStream cannot be opened on a time-series collection because it's a view.
assert.commandFailedWithCode(
    testDB.runCommand({aggregate: tsColl.getName(), pipeline: [{$changeStream: {}}], cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView);

rst.stopSet();
})();
