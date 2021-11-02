'use strict';

/**
 * This test verifies that neither index creation nor find cmd operation on a time-series collection
 * leads to incorrect data results.
 *
 * Creates a time-series collection and populates some data in it. Then creates and drops indexes on
 * the time and meta fields concurrently with queries. Confirms that queries on the time and meta
 * fields return the expected document results. The queries will exercise the find cmd on indexes or
 * collection scans (if no index at the moment of execution). The query results should be the same
 * regardless, the indexes should return the same data as collection scans.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

load("jstests/core/timeseries/libs/timeseries.js");

var $config = (function() {
    // Hardcode time-series collection information so that the threads can all obtain it and run on
    // the same fields and indexes.
    const timeFieldName = "tm";
    const metaFieldName = "mm";
    const metaIndexKey = "mm.a";
    const timeIndexSpec = {"tm": 1};
    const metaIndexSpec = {"mm.a": 1};

    const numDocs = 10;
    // Hardcode 10 time values so that the times are known for the queries to use.
    // Note: times-series collection implementation batches writes by hour into the same bucket. The
    // timestamps selected are deliberately provoking the use of multiple buckets.
    const docTimes = [
        ISODate("2021-01-20T00:00:00.000Z"),
        ISODate("2021-01-20T00:10:00.000Z"),
        ISODate("2021-01-20T00:20:00.000Z"),
        ISODate("2021-01-20T00:30:00.000Z"),
        ISODate("2021-01-20T00:40:00.000Z"),
        ISODate("2021-01-20T00:50:00.000Z"),
        ISODate("2021-01-20T01:00:00.000Z"),
        ISODate("2021-01-20T01:10:00.000Z"),
        ISODate("2021-01-20T01:20:00.000Z"),
        ISODate("2021-01-20T02:40:00.000Z"),
    ];

    // Support for time-series collections may or may not be turned on in the server. This value
    // will be initialized during the 'init' state so that the rest of the states can opt to do
    // nothing when disabled, since testing an inactive feature is pointless.
    let data = {
        supportsTimeseriesCollections: false,
    };

    function getCollectionName(collName) {
        return "find_cmd_with_indexes_timeseries_" + collName;
    }

    /**
     * Checks that the dropIndex cmd result either succeeded or failed in an acceptible manner.
     */
    function processDropIndex(dropIndexRes, indexSpec) {
        assertAlways(dropIndexRes.ok == 1 || dropIndexRes.code == ErrorCodes.IndexNotFound,
                     "Drop index for spec '" + indexSpec + "' failed: " + tojson(dropIndexRes));
    }

    /**
     * Checks that the createIndex cmd result either succeeded or failed in an acceptible manner.
     */
    function processCreateIndex(createIndexRes, indexSpec) {
        assertAlways(createIndexRes.ok == 1 ||
                         createIndexRes.code == ErrorCodes.IndexAlreadyExists ||
                         createIndexRes.code == ErrorCodes.IndexBuildAborted ||
                         createIndexRes.code == ErrorCodes.NoMatchingDocument,
                     "Create index for spec '" + indexSpec + "'failed: " + tojson(createIndexRes));
    }

    const states = {
        /**
         * Figures out whether the server supports time-series collections and sets
         * 'supportsTimeseriesCollections' accordingly.
         */
        init: function init(db, collName) {
            if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
                return;
            }
            this.supportsTimeseriesCollections = true;
        },

        /**
         * Creates an index 'timeIndexSpec' on the time-series collection.
         */
        createTimeIndex: function createTimeIndex(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            const createIndexRes = coll.createIndex(timeIndexSpec);
            processCreateIndex(createIndexRes, timeIndexSpec);
        },

        /**
         * Drops the index 'timeIndexSpec' on the time-series collection.
         */
        dropTimeIndex: function dropTimeIndex(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            const dropIndexRes = coll.dropIndex(timeIndexSpec);
            processDropIndex(dropIndexRes, timeIndexSpec);
        },

        /**
         * Creates an index 'metaIndexSpec' on the time-series collection.
         */
        createMetaIndex: function createMetaIndex(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            const createIndexRes = coll.createIndex(metaIndexSpec);
            processCreateIndex(createIndexRes, metaIndexSpec);
        },

        /**
         * Drops the index 'metaIndexSpec' on the time-series collection.
         */
        dropMetaIndex: function dropMetaIndex(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            const dropIndexRes = coll.dropIndex(metaIndexSpec);
            processDropIndex(dropIndexRes, metaIndexSpec);
        },

        /**
         * Queries all of the time-series collection documents by time field 'timeFieldName'. This
         * query will either provoke a collection scan or use the time field index (if present).
         */
        queryTimeAll: function queryTimeAll(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            try {
                const queryDocs = coll.find({[timeFieldName]: {$lte: docTimes[9]}}).toArray();
                assertAlways.eq(numDocs,
                                queryDocs.length,
                                "Failed to find " + numDocs +
                                    " documents with time field greater than '" + docTimes[0] +
                                    "'. Query results: " + tojson(queryDocs));
            } catch (e) {
                // The query may fail because the index got dropped out from under it.
                assertAlways(e.code == ErrorCodes.QueryPlanKilled,
                             "Expected a QueryPlanKilled error, but encountered: " + e.message);
            }
        },

        /**
         * Query a subset of the time-series collection documents by time field. This query will
         * either provoke a collection scan or use the time field index (if present).
         */
        queryTimeSubset: function queryTimeSubset(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            try {
                const queryDocs = coll.find({[timeFieldName]: {$lte: docTimes[4]}}).toArray();
                assertAlways.eq(numDocs / 2,
                                queryDocs.length,
                                "Failed to find " + (numDocs / 2) +
                                    " documents with time field greater than '" + docTimes[5] +
                                    "'. Query results: " + tojson(queryDocs));
            } catch (e) {
                // The query may fail because the index got dropped out from under it.
                assertAlways(e.code == ErrorCodes.QueryPlanKilled,
                             "Expected a QueryPlanKilled error, but encountered: " + e.message);
            }
        },

        /**
         * Query all of the time-series collection documents by meta field. This query will either
         * scan the collection or use the meta field index (if present).
         */
        queryMetaAll: function queryMetaAll(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            try {
                const queryDocs = coll.find({[metaIndexKey]: {$gte: 0}}).toArray();
                assertAlways.eq(
                    numDocs,
                    queryDocs.length,
                    "Failed to find " + numDocs +
                        " documents with meta field greater than or equal to 0. Query results: " +
                        tojson(queryDocs));
            } catch (e) {
                // The query may fail because the index got dropped out from under it.
                assertAlways(e.code == ErrorCodes.QueryPlanKilled,
                             "Expected a QueryPlanKilled error, but encountered: " + e.message);
            }
        },

        /**
         * Query a subset of the time-series collection documents by meta field. This query will
         * either scan the collection or use the meta field index (if present).
         */
        queryMetaSubset: function queryMetaSubset(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            const coll = db.getCollection(getCollectionName(collName));
            try {
                const queryDocs = coll.find({[metaIndexKey]: {$gt: 4}}).toArray();
                assertAlways.eq(numDocs / 2,
                                queryDocs.length,
                                "Failed to find " + (numDocs / 2) +
                                    " documents with meta field greater than 4. Query results: " +
                                    tojson(queryDocs));
            } catch (e) {
                // The query may fail because the index got dropped out from under it.
                assertAlways(e.code == ErrorCodes.QueryPlanKilled,
                             "Expected a QueryPlanKilled error, but encountered: " + e.message);
            }
        },

    };

    /**
     * Creates a time-series collection and pre-populates it with 'numDocs' documents.
     */
    function setup(db, collName, cluster) {
        if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
            jsTestLog("Skipping test because the time-series collection feature flag is disabled");
            return;
        }

        // Create the collection.
        assertAlways.commandWorked(db.createCollection(getCollectionName(collName), {
            timeseries: {
                timeField: timeFieldName,
                metaField: metaFieldName,
            }
        }));

        // Populate numDocs documents to query.
        const coll = db.getCollection(getCollectionName(collName));
        for (let i = 0; i < numDocs; ++i) {
            // Insert a document with the current time.
            const res =
                coll.insert({_id: i, [timeFieldName]: docTimes[i], [metaFieldName]: {a: i}});
            assertAlways.commandWorked(res);
            assertAlways.eq(1, res.nInserted, tojson(res));
        }
    }

    const standardTransition = {
        createTimeIndex: 0.2,
        dropTimeIndex: 0.05,
        createMetaIndex: 0.2,
        dropMetaIndex: 0.05,
        queryTimeAll: 0.125,
        queryTimeSubset: 0.125,
        queryMetaAll: 0.125,
        queryMetaSubset: 0.125,
    };

    const transitions = {
        init: standardTransition,
        createTimeIndex: standardTransition,
        dropTimeIndex: standardTransition,
        createMetaIndex: standardTransition,
        dropMetaIndex: standardTransition,
        queryTimeAll: standardTransition,
        queryTimeSubset: standardTransition,
        queryMetaAll: standardTransition,
        queryMetaSubset: standardTransition,
    };

    return {
        threadCount: 5,
        iterations: 700,
        states: states,
        data: data,
        transitions: transitions,
        setup: setup,
    };
})();
