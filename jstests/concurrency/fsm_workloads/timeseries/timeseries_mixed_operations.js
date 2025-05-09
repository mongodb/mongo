/**
 * Runs operations against a time-series view and bucket collection simultaneously.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_timeseries,
 *   # TODO SERVER-104916 review the following tag
 *   does_not_support_viewless_timeseries_yet,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

export const $config = (function() {
    const initData = {
        getCollectionName: function(collName) {
            return jsTestName() + '_' + collName;
        },

        getCollection: function(db, collName) {
            return db.getCollection(this.getCollectionName(collName));
        },
    };

    const timeFieldName = "time";
    const metaFieldName = "meta";
    const batchSize = 10;

    const states = {
        init: function init(db, collName) {
            const coll = this.getCollection(db, collName);
            const res = coll.insert({
                [metaFieldName]: 1,
                [timeFieldName]: new Date(),
                first: true,
            });
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(1, res.nInserted, tojson(res));

            // Cache the collection name and command options to use for rawData in order to prevent
            // the workload threads from having to re-determine them during their execution.
            this.collNameForRawOps =
                getTimeseriesCollForRawOps(db, this.getCollectionName(collName));
            this.rawOperationSpec = getRawOperationSpec(db);
        },

        insertManyOrdered: function insertManyOrdered(db, collName) {
            const coll = this.getCollection(db, collName);
            const docs = [];
            for (let i = 0; i < batchSize; i++) {
                docs.push({
                    [metaFieldName]: Random.randInt(this.threadCount),
                    [timeFieldName]: new Date(),
                    data: Random.rand(),
                });
            }
            const res = coll.insertMany(docs, {ordered: true});
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(res.insertedIds.length, batchSize);
        },

        insertManyUnordered: function insertManyUnordered(db, collName) {
            const coll = this.getCollection(db, collName);
            const docs = [];
            for (let i = 0; i < batchSize; i++) {
                docs.push({
                    [metaFieldName]: Random.randInt(this.threadCount),
                    [timeFieldName]: new Date(),
                    data: Random.rand(),
                });
            }
            const res = coll.insertMany(docs, {ordered: false});
            TimeseriesTest.assertInsertWorked(res);
            assert.eq(res.insertedIds.length, batchSize);
        },

        deleteAllBuckets: function deleteAllBuckets(db, collName) {
            assert.commandWorked(db[this.collNameForRawOps].remove({}, this.rawOperationSpec));
        },
    };

    function setup(db, collName, cluster) {
        collName = this.getCollectionName(collName);
        assert.commandWorked(db.createCollection(collName, {
            timeseries: {
                timeField: timeFieldName,
                metaField: metaFieldName,
            }
        }));
    }

    const standardTransition = {
        insertManyOrdered: 0.4,
        insertManyUnordered: 0.4,
        deleteAllBuckets: 0.2,
    };

    const transitions = {
        init: standardTransition,
        insertManyOrdered: standardTransition,
        insertManyUnordered: standardTransition,
        deleteAllBuckets: standardTransition,
    };

    return {
        threadCount: 10,
        iterations: 500,
        startState: 'init',
        states: states,
        data: initData,
        transitions: transitions,
        setup: setup,
    };
})();
