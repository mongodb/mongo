/**
 * Tests that the system.views collection cannot be dropped if time-series collections are present.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const testDB = db.getSiblingDB("timeseries_system_views_drop");

TimeseriesTest.run((insert) => {
    const tsColl = testDB.timeseries_system_views_drop;
    const timeFieldName = 'time';

    tsColl.drop();
    assert.commandWorked(
        testDB.createCollection(tsColl.getName(), {timeseries: {timeField: timeFieldName}}));

    assert.eq(testDB.system.views.find().toArray().length, 1);
    assert.throws(() => {
        testDB.system.views.drop();
    });

    assert(tsColl.drop());
    assert.eq(testDB.system.views.find().toArray().length, 0);

    const coll = testDB.my_coll;

    coll.drop();
    assert.commandWorked(testDB.createCollection(coll.getName()));
    assert.commandWorked(testDB.createView("myView", coll.getName(), []));

    assert(testDB.system.views.drop());
});
})();
