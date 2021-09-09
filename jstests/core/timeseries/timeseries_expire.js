/**
 * Tests that a time-series collection created with the 'expireAfterSeconds' option will remove
 * buckets older than 'expireAfterSeconds' based on the bucket creation time.
 * @tags: [
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const coll = db.timeseries_expire;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    coll.drop();

    const timeFieldName = 'time';
    const expireAfterSeconds = NumberLong(5);
    assert.commandWorked(db.createCollection(
        coll.getName(),
        {timeseries: {timeField: timeFieldName}, expireAfterSeconds: expireAfterSeconds}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    // Inserts a measurement with a time in the past to ensure the measurement will be removed
    // immediately.
    const t = ISODate("2020-11-13T01:00:00Z");
    let start = ISODate();
    assert.lt(t, start);

    const doc = {_id: 0, [timeFieldName]: t, x: 0};
    assert.commandWorked(insert(coll, doc), 'failed to insert doc: ' + tojson(doc));
    jsTestLog('Insertion took ' + ((new Date()).getTime() - start.getTime()) + ' ms.');

    // Wait for the document to be removed.
    start = ISODate();
    assert.soon(() => {
        return 0 == coll.find().itcount();
    });
    jsTestLog('Removal took ' + ((new Date()).getTime() - start.getTime()) + ' ms.');

    // Check bucket collection.
    const bucketDocs = bucketsColl.find().sort({'control.min._id': 1}).toArray();
    assert.eq(0, bucketDocs.length, bucketDocs);
});
})();
