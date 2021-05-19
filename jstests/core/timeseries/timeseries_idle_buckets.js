/**
 * Tests that idle buckets are removed when the bucket catalog's memory threshold is reached.
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

TimeseriesTest.run((insert) => {
    const coll = db.timeseries_idle_buckets;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'time';
    const metaFieldName = 'meta';

    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    // Insert enough documents with large enough metadata so that the bucket catalog memory
    // threshold is reached and idle buckets are expired.
    const numDocs = 100;
    const metaValue = 'a'.repeat(1024 * 1024);
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(insert(
            coll, {[timeFieldName]: ISODate(), [metaFieldName]: {[i.toString()]: metaValue}}));
    }

    // Insert a document with the metadata of a bucket which should have been expired. Thus, a new
    // bucket will be created.
    assert.commandWorked(
        insert(coll, {[timeFieldName]: ISODate(), [metaFieldName]: {0: metaValue}}));
    let bucketDocs = bucketsColl.find({meta: {0: metaValue}}).toArray();
    assert.eq(
        bucketDocs.length, 2, 'Invalid number of buckets for metadata 0: ' + tojson(bucketDocs));

    // Insert a document with the metadata of a bucket with should still be open. Thus, the existing
    // bucket will be used.
    assert.commandWorked(
        insert(coll, {[timeFieldName]: ISODate(), [metaFieldName]: {[numDocs - 1]: metaValue}}));
    bucketDocs = bucketsColl.find({meta: {[numDocs - 1]: metaValue}}).toArray();
    assert.eq(
        bucketDocs.length,
        1,
        'Invalid number of buckets for metadata ' + (numDocs - 1) + ': ' + tojson(bucketDocs));
});
})();
