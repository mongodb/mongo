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
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_inserts.js';

const numValues = 10;

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.init = function(db, collName, connCache) {
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
        const shardedColl = db[collName];
        const updateField = this.metaField + ".tid" + this.tid;
        const oldValue = Random.randInt(numValues);

        // Updates some measurements along the field owned by this thread in both sharded and
        // unsharded ts collections.
        jsTestLog("Executing update state on: " + collName + " on field " + updateField);
        assertAlways.commandWorked(shardedColl.update(
            {[updateField]: {$gte: oldValue}}, {$inc: {[updateField]: 1}}, {multi: true}));
    };

    $config.data.validateCollection = function validate(db, collName) {
        // Since we can't use a 'snapshot' read concern for timeseries updates, updates on the
        // sharded collection may not see the exact same records as the non-sharded, so the
        // validation needs to be more lenient.
        const count = db[collName].find().itcount();
        const countNonSharded = db[this.nonShardCollName].find().itcount();
        assertAlways.eq(
            count,
            countNonSharded,
            "Expected sharded collection to have the same number of records as unsharded");
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 0.4, moveChunk: 0.1, update: 0.5},
        update: {insert: 0.5, moveChunk: 0.1, update: 0.4},
        moveChunk: {insert: 0.4, moveChunk: 0.1, update: 0.5},
    };

    // Reduced iteration and document counts to avoid timeouts.
    $config.iterations = 20;

    // Five minutes.
    $config.data.increment = 1000 * 60 * 5;

    // This should generate documents for a span of one month.
    $config.data.numInitialDocs = 12 * 24 * 30;

    return $config;
});
