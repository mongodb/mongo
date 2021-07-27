/**
 * Tests running the delete command on a time-series collection closes the in-memory bucket.
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
