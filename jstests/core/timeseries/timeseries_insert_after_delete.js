/**
 * Tests running the delete command on a time-series collection closes the in-memory bucket.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.
load("jstests/libs/fixture_helpers.js");             // For 'FixtureHelpers'.

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

if (FixtureHelpers.isMongos(db) &&
    !TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog(
        "Skipping test because the sharded time-series updates and deletes feature flag is disabled");
    return;
}

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB.getCollection('t');
    const timeFieldName = "time";
    const metaFieldName = "tag";

    const objA = {[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: "A"};
    const objB = {[timeFieldName]: ISODate("2021-01-01T01:01:00Z"), [metaFieldName]: "A"};

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(insert(coll, objA));
    assert.eq(assert.commandWorked(testDB.runCommand(
                  {delete: coll.getName(), deletes: [{q: {[metaFieldName]: "A"}, limit: 0}]}))["n"],
              1);
    assert.commandWorked(insert(coll, [objB]));
    const docs = coll.find({}, {_id: 0}).toArray();
    assert.docEq(docs, [objB]);
    assert(coll.drop());
});
})();
