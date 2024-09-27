/**
 * Extends random_moveChunk_timeseries_delete.js workload with findAndModify {remove: true}. Tests
 * deletes in the presence of concurrent insert and moveChunk commands.
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  # Time-series findAndModify does not support retryable writes.
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  featureFlagTimeseriesUpdatesSupport,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_timeseries_deletes.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.doFindAndRemove = function doFindAndRemove(db, collName, connCache) {
        const fieldNameF = "f";
        const fieldNameTid = `tid${this.tid}`;
        const filterFieldName = `${fieldNameF}.${fieldNameTid}`;
        const filterFieldVal = Random.randInt($config.data.numMetaCount);
        const filter = {
            [filterFieldName]: {
                $gte: filterFieldVal,
            },
        };
        // May delete different measurements from the two collections.
        const res1 = assert.commandWorked(
            db.runCommand({findAndModify: collName, query: filter, remove: true}));
        const res2 = assert.commandWorked(
            db.runCommand({findAndModify: this.nonShardCollName, query: filter, remove: true}));
        if (res1 && res1.lastErrorObject.n) {
            assert(res1.value[fieldNameF][fieldNameTid] >= filterFieldVal,
                   `Deleted measurement ${tojson(res1.value)} should match the query predicate ${
                       tojson(filter)}} for the sharded collection`);
        }
        if (res2 && res2.lastErrorObject.n) {
            assert(res2.value[fieldNameF][fieldNameTid] >= filterFieldVal,
                   `Deleted measurement ${tojson(res2.value)} should match the query predicate ${
                       tojson(filter)}} for the non-sharded collection`);
        }
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 1, doFindAndRemove: 3, moveChunk: 1},
        doFindAndRemove: {insert: 1, doFindAndRemove: 3, moveChunk: 1},
        moveChunk: {insert: 1, doFindAndRemove: 1, moveChunk: 0},
    };

    return $config;
});
