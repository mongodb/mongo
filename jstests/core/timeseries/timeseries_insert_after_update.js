/**
 * Tests running the update command on a time-series collection closes any in-memory buckets that
 * were updated.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_multi_updates,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fixture_helpers.js");

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

if (FixtureHelpers.isMongos(db) &&
    !TimeseriesTest.shardedtimeseriesCollectionsEnabled(db.getMongo())) {
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
    const coll = testDB["t"];
    const bucketsColl = testDB["system.buckets." + coll.getName()];

    const timeFieldName = "time";
    const metaFieldName = "m";

    const docs = [
        {[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: "a"},
        {[timeFieldName]: ISODate("2021-01-01T01:01:00Z"), [metaFieldName]: "a"},
        {[timeFieldName]: ISODate("2021-01-01T01:02:00Z"), [metaFieldName]: "b"},
    ];

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(insert(coll, docs[0]));
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [{q: {[metaFieldName]: "a"}, u: {$set: {[metaFieldName]: "b"}}, multi: true}]
    }));
    docs[0][metaFieldName] = "b";

    assert.commandWorked(insert(coll, docs.slice(1)));
    assert.docEq(coll.find({}, {_id: 0}).sort({[timeFieldName]: 1}).toArray(), docs);

    // All three documents should be in their own buckets.
    assert.eq(bucketsColl.find().itcount(), 3, bucketsColl.find().toArray());
});
})();
