/**
 * Tests the updates into sharded time-series collection during a chunk migration. To ensure the
 * correctness, the test does the same writes into an unsharded collection and verifies that the
 * number of documents remain the same at the end. This test also checks that indexes on the
 * time-series buckets collection remain consistent after the test run.
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_fcv_51,
 * ]
 */
'use strict';

const numValues = 10;

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.
load('jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_inserts.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.states.init = function(db, collName, connCache) {
        if (TimeseriesTest.timeseriesCollectionsEnabled(db) &&
            TimeseriesTest.shardedtimeseriesCollectionsEnabled(db) &&
            TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db) &&
            TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(db)) {
            this.featureFlagDisabled = false;
        } else {
            jsTestLog(
                "Skipping executing this test as the requisite feature flags are not enabled.");
        }

        $super.states.init(db, collName);
    };

    $config.data.generateMetaFieldValueForInitialInserts = () => {
        let meta = {};
        // Insert a document with a field for every thread to test concurrent updates on the
        // same document.
        for (let i = 0; i < $config.threadCount; i++) {
            meta["tid" + i] = Random.randInt(numValues);
        }
        return meta;
    };

    $config.data.generateMetaFieldValueForInsertStage = (tid) => {
        return {["tid" + tid]: Random.randInt(numValues)};
    };

    $config.states.update = function(db, collName, connCache) {
        if (this.featureFlagDisabled) {
            return;
        }

        const shardedColl = db[collName];
        const unshardedColl = db[this.nonShardCollName];
        const updateField = "tid" + this.tid;
        const oldValue = Random.randInt(numValues);

        // Updates some measurements along the field owned by this thread in both sharded and
        // unsharded ts collections.
        jsTestLog("Executing update state on: " + collName + " on field " + updateField);
        assertAlways.commandWorked(
            shardedColl.update({[this.metaField]: {[updateField]: {$gte: oldValue}}},
                               {$inc: {[this.metaField + "." + updateField]: 1}},
                               {multi: true}));
        assertAlways.commandWorked(
            unshardedColl.update({[this.metaField]: {[updateField]: {$gte: oldValue}}},
                                 {$inc: {[this.metaField + "." + updateField]: 1}},
                                 {multi: true}));
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 0.4, moveChunk: 0.1, update: 0.5},
        update: {insert: 0.5, moveChunk: 0.1, update: 0.4},
        moveChunk: {insert: 0.4, moveChunk: 0.1, update: 0.5},
    };

    return $config;
});
