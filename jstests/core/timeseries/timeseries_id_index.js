/**
 * Verifies that the _id index cannot be created on a time-series collection.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
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

const coll = db.timeseries_id_index;
coll.drop();

const timeFieldName = "time";
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const bucketsColl = db.getCollection("system.buckets." + coll.getName());

const res = bucketsColl.createIndex({"_id": 1});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);
assert(res.errmsg.includes("cannot have an _id index on a time-series bucket collection"));
})();
