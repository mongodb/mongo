/**
 * Tests running the delete command on a time-series collection with a closed bucket.
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_getmore,
 *   requires_fcv_51,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB.getCollection('t');
    const timeFieldName = "time";
    const metaFieldName = "tag";
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

    const objA = {[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: "A"};
    const objB = {[timeFieldName]: ISODate("2021-01-01T02:00:00Z"), [metaFieldName]: "A"};

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    insert(coll, [objA, objB]);

    // Set the control.closed field of one of the buckets as true.
    const updateResult =
        assert.commandWorked(bucketsColl.update({}, {$set: {"control.closed": true}}));
    assert.eq(updateResult.nMatched, 1);
    assert.eq(updateResult.nModified, 1);

    assert.eq(assert.commandWorked(testDB.runCommand(
                  {delete: coll.getName(), deletes: [{q: {[metaFieldName]: "A"}, limit: 0}]}))["n"],
              1);
    assert.docEq(coll.find({}, {_id: 0}).toArray(), [objA]);
    assert(coll.drop());
});
})();
