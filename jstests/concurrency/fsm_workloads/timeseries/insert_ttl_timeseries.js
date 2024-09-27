/**
 * Creates a time-series collection with a short expireAfterSeconds value. Each thread does an
 * insert on each iteration with a time, a metadata field, 'tid', and random measurement, 'data'. At
 * the end, we wait until the first set of documents has been deleted.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_timeseries,
 *   uses_ttl,
 * ]
 */

import {BalancerHelper} from "jstests/concurrency/fsm_workload_helpers/balancer.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

export const $config = (function() {
    const initData = {
        getCollectionName: function(collName) {
            return "insert_ttl_timeseries_" + collName;
        },

        getCollection: function(db, collName) {
            return db.getCollection(this.getCollectionName(collName));
        },
    };

    const timeFieldName = "time";
    const metaFieldName = "tid";
    const ttlSeconds = 3;
    const defaultBucketMaxRangeMs = 3600 * 1000;
    const batchSize = 10;

    // Generates a time in the past that will be expired soon. TTL for time-series collections only
    // expires buckets once the bucket minimum is past the maximum range of the bucket size, in this
    // case one hour.
    const getTime = function() {
        const now = new Date();
        return new Date(now.getTime() - defaultBucketMaxRangeMs);
    };

    const states = {
        init: function init(db, collName) {
            const coll = this.getCollection(db, collName);
            const res = coll.insert({
                [metaFieldName]: this.tid,
                [timeFieldName]: getTime(),
                first: true,
            });
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(1, res.nInserted, tojson(res));
        },

        /**
         * Insert a single measurement for the current thread id.
         */
        insertOne: function insertOne(db, collName) {
            const coll = this.getCollection(db, collName);
            const res = coll.insert({
                [metaFieldName]: this.tid,
                [timeFieldName]: getTime(),
                data: Random.rand(),
            });
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(1, res.nInserted, tojson(res));
        },

        /**
         * Insert an ordered batch for the current thread id. All measurements should end up in the
         * same bucket.
         */
        insertManyOrdered: function insertManyOrdered(db, collName) {
            const coll = this.getCollection(db, collName);
            const docs = [];
            for (let i = 0; i < batchSize; i++) {
                docs.push({
                    [metaFieldName]: this.tid,
                    [timeFieldName]: getTime(),
                    data: Random.rand(),
                });
            }
            const res = coll.insertMany(docs, {ordered: true});
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(res.insertedIds.length, batchSize);
        },

        /**
         * Insert an unordered batch for a specific thread id. All measurements should end up in
         * the same bucket.
         */
        insertManyUnordered: function insertManyUnordered(db, collName) {
            const coll = this.getCollection(db, collName);
            const docs = [];
            for (let i = 0; i < batchSize; i++) {
                docs.push({
                    [metaFieldName]: this.tid,
                    [timeFieldName]: getTime(),
                    data: Random.rand(),
                });
            }
            const res = coll.insertMany(docs, {ordered: false});
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(res.insertedIds.length, batchSize);
        },

        /**
         * Writers are not restricted to insert documents for their thread id. Insert a batch with
         * randomized thread ids to exercise the case where a batch insert results in writes to
         * several different buckets.
         */
        insertManyRandTid: function insertManyRandTid(db, collName) {
            const coll = this.getCollection(db, collName);
            const docs = [];
            for (let i = 0; i < batchSize; i++) {
                docs.push({
                    [metaFieldName]: Random.randInt(this.threadCount),
                    [timeFieldName]: getTime(),
                    data: Random.rand(),
                });
            }
            const res = coll.insertMany(docs, {ordered: false});
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(res.insertedIds.length, batchSize);
        },

        /**
         * Insert a batch for the current thread id but with older times that should be inserted in
         * different buckets.
         */
        insertManyOld: function insertManyOld(db, collName) {
            const coll = this.getCollection(db, collName);
            const docs = [];
            const start = getTime();
            for (let i = 0; i < batchSize; i++) {
                let time = new Date(start.getTime() - ((batchSize - i) * defaultBucketMaxRangeMs));
                docs.push({
                    [metaFieldName]: this.tid,
                    [timeFieldName]: time,
                    data: Random.rand(),
                });
            }
            const res = coll.insertMany(docs, {ordered: false});
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(res.insertedIds.length, batchSize);
        }
    };

    function setup(db, collName, cluster) {
        collName = this.getCollectionName(collName);
        assert.commandWorked(db.createCollection(collName, {
            timeseries: {
                timeField: timeFieldName,
                metaField: metaFieldName,
            },
            expireAfterSeconds: ttlSeconds,
        }));
    }

    function teardown(db, collName, cluster) {
        if (TestData.runningWithBalancer) {
            // Stop balancing  so that it does not cause the TTLMonitor to fail rounds due to
            // ongoing migration critical sections. TTLMonitor will retry on the next round, but it
            // might not converge in time for the following assertion to pass.
            assert.commandWorked(db.adminCommand({balancerStop: 1, maxTimeMS: 600000}));
            BalancerHelper.joinBalancerRound(db);
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
        collName = this.getCollectionName(collName);
        assert.soon(() => {
            return db[collName].find({first: true}).itcount() == 0;
        }, 'Expected oldest documents to be removed', timeoutMS);

        if (TestData.runningWithBalancer) {
            assert.commandWorked(db.adminCommand({balancerStart: 1, maxTimeMS: 600000}));
            BalancerHelper.enableBalancerForCollection(db, db[collName].getFullName());
        }
    }

    const standardTransition = {
        insertOne: 0.4,
        insertManyOrdered: 0.1,
        insertManyUnordered: 0.1,
        insertManyRandTid: 0.1,
        insertManyOld: 0.1
    };

    const transitions = {
        init: standardTransition,
        insertOne: standardTransition,
        insertManyOrdered: standardTransition,
        insertManyUnordered: standardTransition,
        insertManyRandTid: standardTransition,
        insertManyOld: standardTransition,
    };

    return {
        threadCount: 20,
        iterations: 150,
        startState: 'init',
        states: states,
        data: initData,
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };
})();
