'use strict';

/**
 * Creates a time-series collection with a short expireAfterSeconds value. Each thread does an
 * insert on each iteration with the current time and its thread id. At the end, we wait until the
 * first set of documents has been deleted.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_fcv_49,
 *   requires_timeseries,
 *   sbe_incompatible,
 *   uses_ttl,
 * ]
 */

load("jstests/core/timeseries/libs/timeseries.js");

var $config = (function() {
    const initData = {
        supportsTimeseriesCollections: false,
    };
    const timeFieldName = "time";
    const metaFieldName = "tid";
    const ttlSeconds = 3;
    const defaultBucketMaxRangeMs = 3600 * 1000;

    const getCollectionName = function(collName) {
        return "insert_ttl_timeseries_" + collName;
    };

    // Generates a time in the past that will be expired soon. TTL for time-series collections only
    // expires buckets once the bucket minimum is past the maximum range of the bucket size, in this
    // case one hour.
    const getTime = function() {
        const now = new Date();
        return new Date(now.getTime() - defaultBucketMaxRangeMs);
    };

    const states = {
        init: function init(db, collName) {
            if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
                return;
            }
            this.supportsTimeseriesCollections = true;

            collName = getCollectionName(collName);
            const coll = db.getCollection(collName);
            const res = coll.insert({
                [metaFieldName]: this.tid,
                [timeFieldName]: getTime(),
                first: true,
            });
            assertAlways.commandWorked(res);
            assertAlways.eq(1, res.nInserted, tojson(res));
        },

        insert: function insert(db, collName) {
            if (!this.supportsTimeseriesCollections) {
                return;
            }

            collName = getCollectionName(collName);
            const coll = db.getCollection(collName);
            const res = coll.insert({
                [metaFieldName]: this.tid,
                [timeFieldName]: getTime(),
                data: Math.random(),
            });
            assertAlways.commandWorked(res);
            assertAlways.eq(1, res.nInserted, tojson(res));
        }
    };

    function setup(db, collName, cluster) {
        if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
            jsTestLog("Skipping test because the time-series collection feature flag is disabled");
            return;
        }

        collName = getCollectionName(collName);
        assertAlways.commandWorked(db.createCollection(collName, {
            timeseries: {
                timeField: timeFieldName,
                metaField: metaFieldName,
                expireAfterSeconds: ttlSeconds,
            }
        }));
    }

    function teardown(db, collName, cluster) {
        if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
            return;
        }

        // Default TTL monitor period
        const ttlMonitorSleepSecs = 60;

        // We need to wait for the initial documents to expire. It's possible for this code to
        // run right after the TTL thread has started to sleep, which requires us to wait another
        // period for it to wake up and delete the expired documents. We wait at least another
        // period just to avoid race-prone tests on overloaded test hosts.
        const timeoutMS =
            (TestData.inEvergreen ? 10 : 2) * Math.max(ttlMonitorSleepSecs, ttlSeconds) * 1000;

        print("Waiting for data to be deleted by TTL monitor");
        collName = getCollectionName(collName);
        assertAlways.soon(() => {
            return db[collName].find({first: true}).itcount() == 0;
        }, 'Expected oldest documents to be removed', timeoutMS);
    }

    const transitions = {
        init: {insert: 1},
        insert: {insert: 1},
    };

    return {
        threadCount: 20,
        iterations: 200,
        states: states,
        data: initData,
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };
})();
