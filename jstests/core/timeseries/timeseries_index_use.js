/**
 * Tests index usage on meta and time fields for timeseries collections.
 *
 * @tags: [
 *     # The shardCollection implicitly creates an index on time field.
 *     assumes_unsharded_collection,
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
load("jstests/libs/analyze_plan.js");

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    // Create timeseries collection.
    const timeFieldName = 'time';
    const metaFieldName = 'meta';
    const coll = testDB.getCollection('t');
    const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

    /**
     * Sets up an empty time-series collection with options 'collOpts' on namespace 't' using
     * 'timeFieldName' and 'metaFieldName'. Checks that the buckets collection is created, as well.
     */
    function resetCollections(collOpts = {}) {
        coll.drop();  // implicitly drops bucketsColl.

        assert.commandWorked(testDB.createCollection(
            coll.getName(),
            Object.assign({timeseries: {timeField: timeFieldName, metaField: metaFieldName}},
                          collOpts)));

        const dbCollNames = testDB.getCollectionNames();
        assert.contains(bucketsColl.getName(),
                        dbCollNames,
                        "Failed to find namespace '" + bucketsColl.getName() +
                            "' amongst: " + tojson(dbCollNames));
    }

    /**
     * Creates the index specified by the spec and options, then explains the query to ensure that
     * the created index is used. Runs the query and verifies that the expected number of documents
     * are matched. Finally, deletes the created index.
     */
    const testQueryUsesIndex = function(
        filter, numMatches, indexSpec, indexOpts = {}, queryOpts = {}) {
        assert.commandWorked(
            coll.createIndex(indexSpec, Object.assign({name: "testIndexName"}, indexOpts)));

        let query = coll.find(filter);
        if (queryOpts.collation)
            query = query.collation(queryOpts.collation);

        assert.eq(numMatches, query.itcount());

        const explain = query.explain();
        const ixscan = getAggPlanStage(explain, "IXSCAN");
        assert.neq(null, ixscan, tojson(explain));
        assert.eq("testIndexName", ixscan.indexName, tojson(ixscan));

        assert.commandWorked(coll.dropIndex("testIndexName"));
    };

    /********************************** Tests scalar meta values **********************************/
    resetCollections();
    assert.commandWorked(insert(coll, [
        {_id: 0, [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'), [metaFieldName]: 2},
        {_id: 1, [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'), [metaFieldName]: 3},
        {_id: 2, [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'), [metaFieldName]: 2}
    ]));

    const timeDate = ISODate('2005-01-01 00:00:00.000Z');

    // Test ascending and descending index on timeField.
    testQueryUsesIndex({'time': {$lte: timeDate}}, 2, {'time': 1});
    testQueryUsesIndex({'time': {$gte: timeDate}}, 1, {'time': -1});

    // Test ascending and descending index on metaField.
    testQueryUsesIndex({'meta': {$eq: 3}}, 1, {'meta': 1});
    testQueryUsesIndex({'meta': {$lt: 3}}, 2, {'meta': -1});

    // Test compound indexes on metaField and timeField.
    testQueryUsesIndex({'meta': {$gte: 2}}, 3, {'meta': 1, 'time': 1});
    testQueryUsesIndex({'time': {$lt: timeDate}}, 2, {'time': 1, 'meta': 1});
    testQueryUsesIndex({'meta': {$lte: 3}, 'time': {$gte: timeDate}}, 1, {'meta': 1, 'time': 1});
    testQueryUsesIndex({'meta': {$eq: 2}, 'time': {$lte: timeDate}}, 1, {'meta': -1, 'time': -1});
    testQueryUsesIndex({'meta': {$lt: 3}, 'time': {$lte: timeDate}}, 1, {'time': 1, 'meta': -1});

    /********************************** Tests object meta values **********************************/
    resetCollections();
    assert.commandWorked(insert(coll, [
        {_id: 0, [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'), [metaFieldName]: {a: 1}},
        {
            _id: 1,
            [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: 4, b: 5}
        },
        {
            _id: 2,
            [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: "1", c: 0}
        }
    ]));

    // Test indexes on subfields of metaField.
    testQueryUsesIndex({'meta.a': {$gt: 3}}, 1, {'meta.a': 1});
    testQueryUsesIndex({'meta.a': {$type: 'string'}}, 1, {'meta.a': -1});
    testQueryUsesIndex({'meta.b': {$gte: 0}}, 1, {'meta.b': 1}, {sparse: true});
    testQueryUsesIndex({'meta': {$eq: {a: 1}}}, 1, {'meta': 1});
    testQueryUsesIndex({'meta': {$in: [{a: 1}, {a: 4, b: 5}]}}, 2, {'meta': 1});

    // Test compound indexes on multiple subfields of metaField.
    testQueryUsesIndex({'meta.a': {$lt: 3}}, 1, {'meta.a': 1, 'meta.b': -1});
    testQueryUsesIndex({'meta.a': {$lt: 5}, 'meta.b': {$eq: 5}}, 1, {'meta.a': -1, 'meta.b': 1});
    testQueryUsesIndex(
        {$or: [{'meta.a': {$eq: 1}}, {'meta.a': {$eq: "1"}}]}, 2, {'meta.a': -1, 'meta.b': -1});
    testQueryUsesIndex({'meta.b': {$lte: 5}}, 1, {'meta.b': 1, 'meta.c': 1}, {sparse: true});
    testQueryUsesIndex(
        {'meta.b': {$lte: 5}, 'meta.c': {$lte: 4}}, 0, {'meta.b': 1, 'meta.c': 1}, {sparse: true});

    // Test compound indexes on timeField and subfields of metaField.
    testQueryUsesIndex({'meta.a': {$gte: 2}}, 1, {'meta.a': 1, 'time': 1});
    testQueryUsesIndex({'time': {$lt: timeDate}}, 2, {'time': 1, 'meta.a': 1});
    testQueryUsesIndex(
        {'meta.a': {$lte: 4}, 'time': {$lte: timeDate}}, 2, {'meta.a': 1, 'time': 1});
    testQueryUsesIndex(
        {'meta.a': {$lte: 4}, 'time': {$lte: timeDate}}, 2, {'meta.a': 1, 'time': -1});
    testQueryUsesIndex(
        {'meta.a': {$eq: "1"}, 'time': {$gt: timeDate}}, 1, {'time': -1, 'meta.a': 1});

    // Test wildcard indexes with metaField.
    testQueryUsesIndex({'meta.a': {$lt: 3}}, 1, {'meta.$**': 1});
    testQueryUsesIndex({'meta.b': {$gt: 3}}, 1, {'meta.$**': 1});

    // Test hashed indexes on metaField.
    testQueryUsesIndex({'meta': {$eq: {a: 1}}}, 1, {'meta': "hashed"});
    testQueryUsesIndex({'meta.a': {$eq: 1}}, 1, {'meta.a': "hashed"});
    testQueryUsesIndex({'meta.a': {$eq: 1}}, 1, {'meta.a': "hashed", 'meta.b': -1});
    testQueryUsesIndex(
        {'meta.a': {$eq: 1}, 'meta.b': {$gt: 0}}, 0, {'meta.a': "hashed", 'meta.b': -1});
    testQueryUsesIndex(
        {'meta.a': {$eq: 1}, 'meta.b': {$gt: 0}}, 0, {'meta.b': -1, 'meta.a': "hashed"});

    /*********************************** Tests array meta values **********************************/
    resetCollections();
    assert.commandWorked(insert(coll, [
        {_id: 0, [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'), [metaFieldName]: [1, 2, 3]},
        {_id: 1, [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'), [metaFieldName]: ['a', 'b']},
        {
            _id: 2,
            [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'),
            [metaFieldName]: [{a: 1}, {b: 0}]
        }
    ]));

    // Test multikey indexes on metaField.
    testQueryUsesIndex({'meta': {$eq: {a: 1}}}, 1, {'meta': 1});
    testQueryUsesIndex({'meta': {$eq: 2}}, 1, {'meta': 1});
    testQueryUsesIndex({'meta': {$gte: {a: 1}}}, 1, {'meta': -1});

    resetCollections();
    assert.commandWorked(insert(coll, [
        {
            _id: 0,
            [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: [1, 2, 3], b: 0}
        },
        {
            _id: 1,
            [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: ['a', 'b'], b: 1}
        },
        {
            _id: 2,
            [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: [{b: 1}, {c: 0}]}
        }
    ]));

    // Test multikey indexes on subfields of metaFields.
    testQueryUsesIndex({'meta.a': {$eq: {b: 1}}}, 1, {'meta.a': 1});
    testQueryUsesIndex({'meta.a': {$eq: 2}}, 1, {'meta.a': 1});
    testQueryUsesIndex({'meta.a': {$gte: {a: 1}}}, 1, {'meta.a': -1});
    testQueryUsesIndex(
        {'meta.a': {$gte: 1}, 'meta.b': {$exists: 1}}, 1, {'meta.a': -1, 'meta.b': 1});

    /*********************************** Tests string meta values *********************************/
    const collation = {collation: {locale: 'en', strength: 1, numericOrdering: true}};

    // Create timeseries collection with a collation.
    resetCollections(collation);
    assert.commandWorked(insert(coll, [
        {
            _id: 0,
            [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'),
            [metaFieldName]: "hello hello"
        },
        {
            _id: 1,
            [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'),
            [metaFieldName]: "hello world"
        },
        {_id: 2, [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'), [metaFieldName]: "bye bye"}
    ]));

    // Test index on metaField when collection collation matches query collation.
    testQueryUsesIndex({'meta': {$eq: "bye bye"}}, 1, {'meta': 1}, {}, collation);
    testQueryUsesIndex({'meta': {$gte: "hello hello"}}, 2, {'meta': -1}, {}, collation);

    resetCollections(collation);
    assert.commandWorked(insert(coll, [
        {
            _id: 0,
            [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: "hello hello", b: "hello"}
        },
        {
            _id: 1,
            [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: "hello world", b: "hello"}
        },
        {
            _id: 2,
            [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'),
            [metaFieldName]: {a: "bye bye", b: "bye"}
        }
    ]));

    // Test index on subfields of metaField when collection collation matches query collation.
    testQueryUsesIndex({'meta.a': {$eq: "bye bye"}}, 1, {'meta.a': 1}, {}, collation);
    testQueryUsesIndex({'meta.b': {$gt: "bye bye"}}, 2, {'meta.b': -1}, {}, collation);
});
})();
