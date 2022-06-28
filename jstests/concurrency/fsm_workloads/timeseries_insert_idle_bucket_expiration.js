'use strict';

/**
 * Tests concurrent time-series inserts, with enough batches and data to force buckets to be closed
 * due to the memory usage threshold.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Timeseries do not support multi-document transactions with inserts.
 *   does_not_support_transactions,
 *   # Stepdowns can cause inserts to fail in some cases in sharded passthroughs and not be
 *   # automatically retried. We aren't sure of the root cause yet, but we are excluding this tests
 *   # from those suites for now.
 *   # TODO (SERVER-67609): Remove this tag, or update the explanation above.
 *   does_not_support_stepdowns,
 * ]
 */

var $config = (function() {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';
    const numDocs = 100;

    function getCollectionName(collName) {
        return jsTestName() + "_" + collName;
    }

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

        insertOrdered: function(db, collNameSuffix) {
            let collName = getCollectionName(collNameSuffix);
            insert(db, collName, this.tid, true);
        },

        insertUnordered: function(db, collNameSuffix) {
            let collName = getCollectionName(collNameSuffix);
            insert(db, collName, this.tid, false);
        },
    };

    const setup = function(db, collNameSuffix, cluster) {
        let collName = getCollectionName(collNameSuffix);
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand(
                {setParameter: 1, timeseriesIdleBucketExpiryMemoryUsageThreshold: 1024}));
        });

        assert.commandWorked(db.createCollection(
            collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    };

    const teardown = function(db, collNameSuffix, cluster) {
        let collName = getCollectionName(collNameSuffix);
        const bucketCatalog = db.serverStatus().bucketCatalog;
        if (bucketCatalog !== undefined) {
            jsTestLog(bucketCatalog);
        }
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
