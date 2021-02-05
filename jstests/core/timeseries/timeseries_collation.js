/**
 * Tests that time-series collections respect collations for metadata and min/max.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_retryable_writes,  # Batches containing more than one measurement
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog('Skipping test because the time-series collection feature flag is disabled');
    return;
}

const testDB = db.getSiblingDB(jsTestName());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'meta';

coll.drop();
assert.commandWorked(testDB.createCollection(coll.getName(), {
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    collation: {locale: 'en', strength: 1, numericOrdering: true}
}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

const docs = [
    {
        _id: 0,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {a: ['b'], c: 'D'},
        x: '10',
        y: {z: ['2']}
    },
    {
        _id: 1,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {a: ['B'], c: 'd'},
        x: '5',
        y: {z: ['5']}
    },
    {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: {a: ['B'], c: 'D'}}
];

assert.commandWorked(coll.insert([docs[0], docs[1]], {ordered: false}));
assert.commandWorked(coll.insert(docs[2], {ordered: false}));

// The metadata of all of the inserted documents matches based on the collation, so when returned
// they will all have the metadata from the document that was inserted first.
const results = coll.find().sort({_id: 1}).toArray();
for (let i = 0; i < results.length; i++) {
    const doc = docs[i];
    doc[metaFieldName] = docs[0][metaFieldName];
    assert.docEq(results[i], doc);
}

const buckets = bucketsColl.find().toArray();
jsTestLog('Checking buckets: ' + tojson(buckets));
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min.x, '5');
assert.eq(buckets[0].control.min.y, {z: ['2']});
assert.eq(buckets[0].control.max.x, '10');
assert.eq(buckets[0].control.max.y, {z: ['5']});
})();