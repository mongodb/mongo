'use strict';

/**
 * Tests concurrent time-series inserts, with enough batches and data to force buckets to be closed
 * due to the memory usage threshold.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

var $config = (function() {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';
    const numDocs = 100;

    const insert = function(db, collName, tid, ordered) {
        const docs = [];
        for (let i = 0; i < numDocs; ++i) {
            docs.push({
                [timeFieldName]: ISODate(),
                [metaFieldName]: (tid * numDocs) + i,
            });
        }
        assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: ordered}));
    };

    const states = {
        init: function(db, collName) {},

        insertOrdered: function(db, collName) {
            insert(db, collName, this.tid, true);
        },

        insertUnordered: function(db, collName) {
            insert(db, collName, this.tid, false);
        },
    };

    const setup = function(db, collName, cluster) {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand(
                {setParameter: 1, timeseriesIdleBucketExpiryMemoryUsageThreshold: 1024}));
        });

        assert.commandWorked(db.createCollection(
            collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    };

    const teardown = function(db, collName, cluster) {
        jsTestLog(db.serverStatus().bucketCatalog);
        jsTestLog(db.runCommand({collStats: collName}).timeseries);

        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                timeseriesIdleBucketExpiryMemoryUsageThreshold: 1024 * 1024 * 100,
            }));
        });
    };

    const standardTransition = {
        insertOrdered: 0.5,
        insertUnordered: 0.5,
    };

    const transitions = {
        init: standardTransition,
        insertOrdered: standardTransition,
        insertUnordered: standardTransition,
    };

    return {
        threadCount: 10,
        iterations: 100,
        setup: setup,
        teardown: teardown,
        states: states,
        transitions: transitions,
    };
})();
