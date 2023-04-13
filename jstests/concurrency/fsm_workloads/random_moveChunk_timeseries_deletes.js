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
            !TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(db);
        if (this.featureFlagDisabled) {
            jsTestLog(
                "Skipping executing this test as the requisite feature flags are not enabled.");
        }

        this.arbitraryDeletesEnabled =
            FeatureFlagUtil.isPresentAndEnabled(db, "TimeseriesDeletesSupport");
    };

    $config.states.doDelete = function doDelete(db, collName, connCache) {
        if (this.featureFlagDisabled) {
            return;
        }

        // Alternate between filtering on the meta field and filtering on a data field. This will
        // cover both the timeseries batch delete and arbitrary delete paths.
        const filterFieldName = !this.arbitraryDeletesEnabled || Random.randInt(2) == 0
            ? "m.tid" + this.tid
            : "f.tid" + this.tid;
        const filter = {
            [filterFieldName]: {
                $gte: Random.randInt($config.data.numMetaCount),
            },
        };
        assertAlways.commandWorked(db[collName].deleteMany(filter));
        assertAlways.commandWorked(db[this.nonShardCollName].deleteMany(filter));
    };

    $config.data.validateCollection = function validate(db, collName) {
        // Since we can't use a 'snapshot' read concern for timeseries deletes, deletes on the
        // sharded collection may not see the exact same records as the non-sharded, so the
        // validation needs to be more lenient.
        const count = db[collName].find().itcount();
        const countNonSharded = db[this.nonShardCollName].find().itcount();
        assertAlways.gte(
            count,
            countNonSharded,
            "Expected sharded collection to have the same or more records than unsharded");
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 3, doDelete: 3, moveChunk: 1},
        doDelete: {insert: 3, doDelete: 3, moveChunk: 1},
        moveChunk: {insert: 1, doDelete: 1, moveChunk: 0},
    };

    return $config;
});
