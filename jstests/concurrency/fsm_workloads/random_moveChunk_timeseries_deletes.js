/**
 * Extends random_moveChunk_timeseries_inserts.js workload with delete stage. Tests deletes in the
 * presence of concurrent insert and moveChunk commands.
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_fcv_51,
 * ]
 */
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');  // For 'TimeseriesTest' helpers.
// Load parent workload for extending below.
load('jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_inserts.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.generateMetaFieldValueForInitialInserts = () => {
        let meta = {};
        // Insert a document with a field for every thread to test concurrent deletes of the
        // same document.
        for (let i = 0; i < $config.threadCount; i++) {
            meta["tid" + i] = Random.randInt($config.data.numMetaCount);
        }
        return meta;
    };

    $config.data.generateMetaFieldValueForInsertStage = (tid) => {
        // After the initial stage, each thread will insert documents containing only fields with
        // this thread's id. This ensures we do not run into concurrency issues with concurrent
        // inserts and deletes.
        // NOTE: This problem does not exist for the initial set of documents, since they are
        //       inserted before any delete operation is issued.
        return {["tid" + tid]: Random.randInt($config.data.numMetaCount)};
    };

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.call(this, db, collName, connCache);

        this.featureFlagDisabled = this.featureFlagDisabled ||
            !TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db) ||
            !TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(db);
        if (this.featureFlagDisabled) {
            jsTestLog(
                "Skipping executing this test as the requisite feature flags are not enabled.");
        }
    };

    $config.states.doDelete = function doDelete(db, collName, connCache) {
        if (this.featureFlagDisabled) {
            return;
        }

        const filter = {
            m: {
                ["tid" + this.tid]: {
                    $gte: Random.randInt($config.data.numMetaCount),
                },
            },
        };
        assertAlways.commandWorked(db[collName].deleteMany(filter));
        assertAlways.commandWorked(db[this.nonShardCollName].deleteMany(filter));
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 3, doDelete: 3, moveChunk: 1},
        doDelete: {insert: 3, doDelete: 3, moveChunk: 1},
        moveChunk: {insert: 1, doDelete: 1, moveChunk: 0},
    };

    return $config;
});
