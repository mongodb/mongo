/**
 * Tests that time-series collections respect collations for metadata and min/max.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');

TimeseriesTest.run((insert) => {
    const coll = db.timeseries_collation;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'time';
    const metaFieldName = 'meta';

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
        collation: {locale: 'en', strength: 1, numericOrdering: true}
    }));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

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
            r: "s",
            x: '5',
            y: {z: ['5']}
        },
        {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: {a: ['B'], c: 'D'}},
        {
            _id: 3,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {a: ['B'], c: 'd'},
            r: "S",
            X: '10',
            Y: {z: ['2']}
        },
    ];

    assert.commandWorked(insert(coll, [docs[0], docs[1]]));
    assert.commandWorked(insert(coll, docs[2]));
    assert.commandWorked(insert(coll, docs[3]));

    // The metadata of all of the inserted documents matches based on the collation. If we were to
    // take collation into account when bucketing, we would end up getting back documents which all
    // share the same metadata, which wouldn't match their original data. So let's make sure all
    // the documents match their original data that we inserted.
    const results = coll.find().sort({_id: 1}).toArray();
    assert.eq(docs.length, results.length);
    for (let i = 0; i < results.length; i++) {
        assert.docEq(results[i], docs[i]);
    }

    // Now let's check that min and max appropriately ignore collation for field names, but not
    // values.
    const buckets = bucketsColl.find().sort({'control.min._id': 1}).toArray();
    jsTestLog('Checking buckets: ' + tojson(buckets));
    assert.eq(buckets.length, 3);
    assert.eq(buckets[0].control.min.x, '10');
    assert.eq(buckets[0].control.min.y, {z: ['2']});
    assert.eq(buckets[0].control.max.x, '10');
    assert.eq(buckets[0].control.max.y, {z: ['2']});
    assert.eq(buckets[1].control.min.r, 's');
    assert.eq(buckets[1].control.min.x, '5');
    assert.eq(buckets[1].control.min.X, '10');
    assert.eq(buckets[1].control.min.y, {z: ['5']});
    assert.eq(buckets[1].control.min.Y, {z: ['2']});
    assert.eq(buckets[1].control.max.r, 's');
    assert.eq(buckets[1].control.max.x, '5');
    assert.eq(buckets[1].control.max.X, '10');
    assert.eq(buckets[1].control.max.y, {z: ['5']});
    assert.eq(buckets[1].control.max.Y, {z: ['2']});
    assert.eq(buckets[2].control.min.x, null);
    assert.eq(buckets[2].control.min.y, null);
    assert.eq(buckets[2].control.max.x, null);
    assert.eq(buckets[2].control.max.y, null);
});
})();
