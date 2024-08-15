/**
 * Same as the base workload, but refines to a nested shard key.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   resource_intensive,
 *   # The init() state function populates each document owned by a particular thread with a
 *   # "counter" value. Doing so may take longer than the configured stepdown interval. It is
 *   # therefore unsafe to automatically run inside a multi-statement transaction because its
 *   # progress will continually be interrupted.
 *   operations_longer_than_stepdown_interval_in_txns,
 *   requires_non_retryable_writes,
 *   requires_sharding,
 *   uses_transactions,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/random_moveChunk_refine_collection_shard_key_broadcast_update_transaction.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.newShardKey = {a: 1, "b.c": 1};
    $config.data.newShardKeyFields = ["a", "b.c"];
    return $config;
});
