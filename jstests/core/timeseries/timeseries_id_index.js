/**
 * Verifies that the _id index can be created on a timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

const coll = db.timeseries_id_index;
coll.drop();

const timeFieldName = "time";
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const bucketsColl = db.getCollection("system.buckets." + coll.getName());

assert.commandWorked(bucketsColl.createIndex({"_id": 1}));
assert.commandWorked(bucketsColl.createIndex({"_id": 1}, {clustered: true, unique: true}));

// Passing 'clustered' without unique, regardless of the type of clustered collection, is illegal.
assert.commandFailedWithCode(bucketsColl.createIndex({"_id": 1}, {clustered: true}),
                             ErrorCodes.CannotCreateIndex);
})();
