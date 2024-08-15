/**
 * Tests the updates into sharded time-series collection during a chunk migration.
 *
 * @tags: [
 *  requires_sharding,
 *  resource_intensive,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_fcv_51,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_inserts.js';

const numValues = 10;

export const $config = extendWorkload($baseConfig, function($config, $super) {
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

    // Perform bucket level updates by updating the meta field of measurements.
    $config.states.bucketLevelUpdate = function(db, collName, connCache) {
        const shardedColl = db[collName];
        const updateField = this.metaField + ".tid" + this.tid;
        const oldValue = Random.randInt(numValues);

        jsTestLog("Executing bucket level update on: " + collName + " on field '" + updateField +
                  "'");
        assert.commandWorked(shardedColl.update(
            {[updateField]: {$gte: oldValue}}, {$inc: {[updateField]: 1}}, {multi: true}));
    };

    $config.data.validateCollection = function validate(db, collName) {
        // Since we can't use a 'snapshot' read concern for timeseries updates, updates on the
        // sharded collection may not see the exact same records as the non-sharded, so the
        // validation needs to be more lenient.
        const pipeline = [{$project: {_id: "$_id"}}, {$sort: {_id: 1}}];
        const diff = DataConsistencyChecker.getDiff(db[collName].aggregate(pipeline),
                                                    db[this.nonShardCollName].aggregate(pipeline));
        assert.eq(diff,
                  {docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []});
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 0.2, moveChunk: 0.1, bucketLevelUpdate: 0.25},
        bucketLevelUpdate: {insert: 0.2, moveChunk: 0.1, bucketLevelUpdate: 0.2},
        moveChunk: {insert: 0.2, moveChunk: 0.1, bucketLevelUpdate: 0.25},
    };

    // Reduced iteration and document counts to avoid timeouts.
    $config.iterations = 20;

    // Five minutes.
    $config.data.increment = 1000 * 60 * 5;

    // This should generate documents for a span of one month.
    $config.data.numInitialDocs = 12 * 24 * 30;

    return $config;
});
