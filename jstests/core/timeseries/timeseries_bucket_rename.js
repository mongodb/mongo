/**
 * Tests that a system.buckets collection cannot be renamed.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

const coll = db.timeseries_bucket_rename;
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(bucketsColl.getName(), db.getCollectionNames());

assert.commandFailedWithCode(db.adminCommand({
    renameCollection: bucketsColl.getFullName(),
    to: db.getName() + ".otherColl",
    dropTarget: false
}),
                             ErrorCodes.IllegalOperation);
})();
