/**
 * Tests index usage on meta and time fields for timeseries collections.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_pipeline_optimization,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");

const generateTest = (useHint) => {
    return (insert) => {
        const testDB = db.getSiblingDB(jsTestName());
        assert.commandWorked(testDB.dropDatabase());

        // Create timeseries collection.
        const timeFieldName = 'ts';
        const metaFieldName = 'mm';
        const coll = testDB.getCollection('t');
        const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

        /**
         * Sets up an empty time-series collection with options 'collOpts' on namespace 't' using
         * 'timeFieldName' and 'metaFieldName'. Checks that the buckets collection is created, as
         * well.
         */
        function resetCollections(collOpts = {}) {
            coll.drop();  // implicitly drops bucketsColl.

            assert.commandWorked(testDB.createCollection(
                coll.getName(),
                Object.assign({timeseries: {timeField: timeFieldName, metaField: metaFieldName}},
                              collOpts)));
            if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db)) {
                // When enabled, the {meta: 1, time: 1} index gets built by default on the
                // time-series bucket collection. When this index is present, the query planner will
                // use it, changing the expected behaviour of this test. Drop the index.
                assert.commandWorked(coll.dropIndex({[metaFieldName]: 1, [timeFieldName]: 1}));
            }

            const dbCollNames = testDB.getCollectionNames();
            assert.contains(bucketsColl.getName(),
                            dbCollNames,
                            "Failed to find namespace '" + bucketsColl.getName() +
                                "' amongst: " + tojson(dbCollNames));
        }

        /**
         * Creates the index specified by the spec and options, then explains the query to ensure
         * that the created index is used. Runs the query and verifies that the expected number of
         * documents are matched. Finally, deletes the created index.
         */
        const testQueryUsesIndex = function(
            filter, numMatches, indexSpec, indexOpts = {}, queryOpts = {}) {
            assert.commandWorked(
                coll.createIndex(indexSpec, Object.assign({name: "testIndexName"}, indexOpts)));

            let query = coll.find(filter);
            if (useHint)
                query = query.hint(indexSpec);
            if (queryOpts.collation)
                query = query.collation(queryOpts.collation);

            assert.eq(numMatches, query.itcount());

            const explain = query.explain();
            const ixscan = getAggPlanStage(explain, "IXSCAN");
            assert.neq(null, ixscan, tojson(explain));
            assert.eq("testIndexName", ixscan.indexName, tojson(ixscan));
            assert.commandWorked(coll.dropIndex("testIndexName"));
        };

        /**
         * Creates the index specified by the spec and options, then explains the query to ensure
         * that the created index is used. Runs the query and verifies that the expected number of
         * documents are matched. Finally, deletes the created index.
         */
        const testAggregationUsesIndex = function(
            pipeline, numMatches, indexSpec, stageType = "IXSCAN", indexOpts = {}) {
            assert.commandWorked(
                coll.createIndex(indexSpec, Object.assign({name: "testIndexName"}, indexOpts)));

            let aggregation = coll.aggregate(pipeline);
            assert.eq(numMatches, aggregation.itcount());

            const options = useHint ? {hint: indexSpec} : {};
            const explain = coll.explain().aggregate(pipeline, options);
            const ixscan = getAggPlanStage(explain, stageType);
            assert.neq(null, ixscan, tojson(explain));
            assert.eq("testIndexName", ixscan.indexName, tojson(ixscan));

            assert.commandWorked(coll.dropIndex("testIndexName"));
        };

        /******************************* Tests scalar meta values *********************************/
        resetCollections();
        assert.commandWorked(insert(coll, [
            {_id: 0, [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'), [metaFieldName]: 2},
            {_id: 1, [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'), [metaFieldName]: 3},
            {_id: 2, [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'), [metaFieldName]: 2}
        ]));

        const timeDate = ISODate('2005-01-01 00:00:00.000Z');

        // Test ascending and descending index on timeField.
        if (!FixtureHelpers.isSharded(bucketsColl)) {
            // Skip if the collection is implicitly sharded: it may use the implicitly created
            // index.
            testQueryUsesIndex({[timeFieldName]: {$lte: timeDate}}, 2, {[timeFieldName]: 1});
            testQueryUsesIndex({[timeFieldName]: {$gte: timeDate}}, 1, {[timeFieldName]: -1});
        }

        // Test ascending and descending index on metaField.
        testQueryUsesIndex({[metaFieldName]: {$eq: 3}}, 1, {[metaFieldName]: 1});
        testQueryUsesIndex({[metaFieldName]: {$lt: 3}}, 2, {[metaFieldName]: -1});

        // Test compound indexes on metaField and timeField.
        if (!FixtureHelpers.isSharded(bucketsColl)) {
            // Skip if the collection is implicitly sharded: it may use the implicitly created
            // index.
            testQueryUsesIndex(
                {[metaFieldName]: {$gte: 2}}, 3, {[metaFieldName]: 1, [timeFieldName]: 1});
            testQueryUsesIndex(
                {[timeFieldName]: {$lt: timeDate}}, 2, {[timeFieldName]: 1, [metaFieldName]: 1});
            testQueryUsesIndex({[metaFieldName]: {$lte: 3}, [timeFieldName]: {$gte: timeDate}},
                               1,
                               {[metaFieldName]: 1, [timeFieldName]: 1});
            testQueryUsesIndex({[metaFieldName]: {$eq: 2}, [timeFieldName]: {$lte: timeDate}},
                               1,
                               {[metaFieldName]: -1, [timeFieldName]: -1});
            testQueryUsesIndex({[metaFieldName]: {$lt: 3}, [timeFieldName]: {$lte: timeDate}},
                               1,
                               {[timeFieldName]: 1, [metaFieldName]: -1});
        }

        /******************************** Tests object meta values ********************************/
        resetCollections();
        assert.commandWorked(insert(coll, [
            {_id: 0, [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'), [metaFieldName]: {a: 1}},
            {
                _id: 1,
                [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'),
                [metaFieldName]: {a: 4, b: 5, loc: [1.0, 2.0]}
            },
            {
                _id: 2,
                [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'),
                [metaFieldName]: {a: "1", c: 0, loc: [-1.0, -2.0]}
            }
        ]));

        // Test indexes on subfields of metaField.
        testQueryUsesIndex({[metaFieldName + '.a']: {$gt: 3}}, 1, {[metaFieldName + '.a']: 1});
        testQueryUsesIndex(
            {[metaFieldName + '.a']: {$type: 'string'}}, 1, {[metaFieldName + '.a']: -1});
        testQueryUsesIndex(
            {[metaFieldName + '.b']: {$gte: 0}}, 1, {[metaFieldName + '.b']: 1}, {sparse: true});
        testQueryUsesIndex({[metaFieldName]: {$eq: {a: 1}}}, 1, {[metaFieldName]: 1});
        testQueryUsesIndex({[metaFieldName]: {$in: [{a: 1}, {a: 4, b: 5, loc: [1.0, 2.0]}]}},
                           2,
                           {[metaFieldName]: 1});

        // Test compound indexes on multiple subfields of metaField.
        testQueryUsesIndex({[metaFieldName + '.a']: {$lt: 3}},
                           1,
                           {[metaFieldName + '.a']: 1, [metaFieldName + '.b']: -1});
        testQueryUsesIndex({[metaFieldName + '.a']: {$lt: 5}, [metaFieldName + '.b']: {$eq: 5}},
                           1,
                           {[metaFieldName + '.a']: -1, [metaFieldName + '.b']: 1});
        testQueryUsesIndex(
            {$or: [{[metaFieldName + '.a']: {$eq: 1}}, {[metaFieldName + '.a']: {$eq: "1"}}]},
            2,
            {[metaFieldName + '.a']: -1, [metaFieldName + '.b']: -1});
        testQueryUsesIndex({[metaFieldName + '.b']: {$lte: 5}},
                           1,
                           {[metaFieldName + '.b']: 1, [metaFieldName + '.c']: 1},
                           {sparse: true});
        testQueryUsesIndex({[metaFieldName + '.b']: {$lte: 5}, [metaFieldName + '.c']: {$lte: 4}},
                           0,
                           {[metaFieldName + '.b']: 1, [metaFieldName + '.c']: 1},
                           {sparse: true});

        // Test compound indexes on timeField and subfields of metaField.
        if (!FixtureHelpers.isSharded(bucketsColl)) {
            // Skip if the collection is implicitly sharded: it may use the implicitly created
            // index.
            testQueryUsesIndex({[metaFieldName + '.a']: {$gte: 2}},
                               1,
                               {[metaFieldName + '.a']: 1, [timeFieldName]: 1});
            testQueryUsesIndex({[timeFieldName]: {$lt: timeDate}},
                               2,
                               {[timeFieldName]: 1, [metaFieldName + '.a']: 1});
            testQueryUsesIndex(
                {[metaFieldName + '.a']: {$lte: 4}, [timeFieldName]: {$lte: timeDate}},
                2,
                {[metaFieldName + '.a']: 1, [timeFieldName]: 1});
            testQueryUsesIndex(
                {[metaFieldName + '.a']: {$lte: 4}, [timeFieldName]: {$lte: timeDate}},
                2,
                {[metaFieldName + '.a']: 1, [timeFieldName]: -1});
            testQueryUsesIndex(
                {[metaFieldName + '.a']: {$eq: "1"}, [timeFieldName]: {$gt: timeDate}},
                1,
                {[timeFieldName]: -1, [metaFieldName + '.a']: 1});
        }

        // Test wildcard indexes with metaField.
        testQueryUsesIndex({[metaFieldName + '.a']: {$lt: 3}}, 1, {[metaFieldName + '.$**']: 1});
        testQueryUsesIndex({[metaFieldName + '.b']: {$gt: 3}}, 1, {[metaFieldName + '.$**']: 1});

        // Test hashed indexes on metaField.
        testQueryUsesIndex({[metaFieldName]: {$eq: {a: 1}}}, 1, {[metaFieldName]: "hashed"});
        testQueryUsesIndex(
            {[metaFieldName + '.a']: {$eq: 1}}, 1, {[metaFieldName + '.a']: "hashed"});
        testQueryUsesIndex({[metaFieldName + '.a']: {$eq: 1}},
                           1,
                           {[metaFieldName + '.a']: "hashed", [metaFieldName + '.b']: -1});
        testQueryUsesIndex({[metaFieldName + '.a']: {$eq: 1}, [metaFieldName + '.b']: {$gt: 0}},
                           0,
                           {[metaFieldName + '.a']: "hashed", [metaFieldName + '.b']: -1});
        testQueryUsesIndex({[metaFieldName + '.a']: {$eq: 1}, [metaFieldName + '.b']: {$gt: 0}},
                           0,
                           {[metaFieldName + '.b']: -1, [metaFieldName + '.a']: "hashed"});

        // Test geo-type indexes on metaField.
        testQueryUsesIndex({
            [metaFieldName + '.loc']: {
                $geoWithin: {
                    $geometry:
                        {type: "Polygon", coordinates: [[[0, 0], [0, 3], [3, 3], [3, 0], [0, 0]]]}
                }
            }
        },
                           1,
                           {[metaFieldName + '.loc']: '2dsphere'});
        testQueryUsesIndex({[metaFieldName + '.loc']: {$geoWithin: {$center: [[1.01, 2.01], 0.1]}}},
                           1,
                           {[metaFieldName + '.loc']: '2d'});
        testAggregationUsesIndex(
            [
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [40.4, -70.4]},
                        distanceField: "dist",
                        spherical: true,
                        key: metaFieldName + '.loc'
                    }
                },
                {$limit: 1}
            ],
            1,
            {[metaFieldName + '.loc']: '2dsphere'},
            "GEO_NEAR_2DSPHERE");
        testAggregationUsesIndex(
            [
                {
                    $geoNear:
                        {near: [40.4, -70.4], distanceField: "dist", key: metaFieldName + '.loc'}
                },
                {$limit: 1}
            ],
            1,
            {[metaFieldName + '.loc']: '2d'},
            "GEO_NEAR_2D");

        /********************************* Tests array meta values ********************************/
        resetCollections();
        assert.commandWorked(insert(coll, [
            {
                _id: 0,
                [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'),
                [metaFieldName]: [1, 2, 3]
            },
            {
                _id: 1,
                [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'),
                [metaFieldName]: ['a', 'b']
            },
            {
                _id: 2,
                [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'),
                [metaFieldName]: [{a: 1}, {b: 0}]
            }
        ]));

        // Test multikey indexes on metaField.
        testQueryUsesIndex({[metaFieldName]: {$eq: {a: 1}}}, 1, {[metaFieldName]: 1});
        testQueryUsesIndex({[metaFieldName]: {$eq: 2}}, 1, {[metaFieldName]: 1});
        testQueryUsesIndex({[metaFieldName]: {$gte: {a: 1}}}, 1, {[metaFieldName]: -1});

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
        testQueryUsesIndex({[metaFieldName + '.a']: {$eq: {b: 1}}}, 1, {[metaFieldName + '.a']: 1});
        testQueryUsesIndex({[metaFieldName + '.a']: {$eq: 2}}, 1, {[metaFieldName + '.a']: 1});
        testQueryUsesIndex(
            {[metaFieldName + '.a']: {$gte: {a: 1}}}, 1, {[metaFieldName + '.a']: -1});
        testQueryUsesIndex(
            {[metaFieldName + '.a']: {$gte: 1}, [metaFieldName + '.b']: {$exists: 1}},
            1,
            {[metaFieldName + '.a']: -1, [metaFieldName + '.b']: 1});

        /********************************* Tests string meta values *******************************/
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
            {
                _id: 2,
                [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'),
                [metaFieldName]: "bye bye"
            }
        ]));

        // Test index on metaField when collection collation matches query collation.
        testQueryUsesIndex(
            {[metaFieldName]: {$eq: "bye bye"}}, 1, {[metaFieldName]: 1}, {}, collation);
        testQueryUsesIndex(
            {[metaFieldName]: {$gte: "hello hello"}}, 2, {[metaFieldName]: -1}, {}, collation);

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
        testQueryUsesIndex({[metaFieldName + '.a']: {$eq: "bye bye"}},
                           1,
                           {[metaFieldName + '.a']: 1},
                           {},
                           collation);
        testQueryUsesIndex({[metaFieldName + '.b']: {$gt: "bye bye"}},
                           2,
                           {[metaFieldName + '.b']: -1},
                           {},
                           collation);

        /*********************************** Tests $expr predicates
         * *********************************/
        resetCollections();
        assert.commandWorked(insert(coll, [
            {_id: 0, [timeFieldName]: ISODate('1990-01-01 00:00:00.000Z'), [metaFieldName]: 2},
            {_id: 1, [timeFieldName]: ISODate('2000-01-01 00:00:00.000Z'), [metaFieldName]: 3},
            {_id: 2, [timeFieldName]: ISODate('2010-01-01 00:00:00.000Z'), [metaFieldName]: 2}
        ]));

        if (!FixtureHelpers.isSharded(bucketsColl)) {
            // Skip if the collection is implicitly sharded: it may use the implicitly created
            // index.
            testAggregationUsesIndex([{
                                         $match: {
                                             $expr: {
                                                 $and: [
                                                     {$gt: ["$" + timeFieldName, timeDate]},
                                                     {$eq: ["$" + metaFieldName, 2]}
                                                 ]
                                             }
                                         }
                                     }],
                                     1,
                                     {[timeFieldName]: 1, [metaFieldName]: 1});
        }
    };
};

// Run the test twice, once without hinting the index, and again hinting the index by spec.
TimeseriesTest.run(generateTest(false));
TimeseriesTest.run(generateTest(true));
})();
