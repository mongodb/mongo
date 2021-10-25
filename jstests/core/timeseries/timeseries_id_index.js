/**
 * Verifies that the _id index cannot be created on a time-series collection.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_getmore,
 * ]
 */
(function() {
"use strict";

const coll = db.timeseries_id_index;
coll.drop();

const timeFieldName = "time";
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const bucketsColl = db.getCollection("system.buckets." + coll.getName());

const res = bucketsColl.createIndex({"_id": 1});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);
assert(res.errmsg.includes("cannot create the _id index on a clustered collection") ||
       res.errmsg.includes("cannot create an _id index on a collection already clustered by _id"));
})();
