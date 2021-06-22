/**
 * Tests running the update command on a time-series collection.
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_50,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB.getCollection('t');
    const timeFieldName = "time";

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    assert.commandWorked(insert(coll, {[timeFieldName]: ISODate(), tag: "A"}));

    assert.commandFailedWithCode(coll.update({}, {$set: {tag: "B"}}), ErrorCodes.IllegalOperation);
});
}());
