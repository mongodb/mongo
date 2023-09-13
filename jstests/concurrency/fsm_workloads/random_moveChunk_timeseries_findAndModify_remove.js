/**
 * Extends random_moveChunk_timeseries_delete.js workload with findAndModify {remove: true}. Tests
 * deletes in the presence of concurrent insert and moveChunk commands.
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  # Time-series findAndModify does not support retryable writes.
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_fcv_72,
 * ]
 */
import {assertAlways} from "jstests/concurrency/fsm_libs/assert.js";
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_deletes.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.doFindAndRemove = function doFindAndRemove(db, collName, connCache) {
        const filterFieldName = "f.tid" + this.tid;
        const filterFieldVal = Random.randInt($config.data.numMetaCount);
        const filter = {
            [filterFieldName]: {
                $gte: filterFieldVal,
            },
        };
        // May delete different measurements from the two collections.
        const res1 = assertAlways.commandWorked(
            db.runCommand({findAndModify: collName, query: filter, remove: true}));
        const res2 = assertAlways.commandWorked(
            db.runCommand({findAndModify: this.nonShardCollName, query: filter, remove: true}));
        if (res1 && res1.lastErrorObject.n) {
            assert(res1.value[filterFieldName] >= filterFieldVal,
                   `Deleted measurement ${res1.value} should match the query predicate ${
                       tojson(filter)}}`);
        }
        if (res2 && res2.lastErrorObject.n) {
            assert(res2.value[filterFieldName] >= filterFieldVal,
                   `Deleted measurement ${res2.value} should match the query predicate ${
                       tojson(filter)}}`);
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
