/**
 * Same as the base workload, but refines to a nested shard key.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_sharding,
 *   assumes_stable_shard_list,
 *   # TODO: SERVER-114503 Investigate DDL commands FSM tests leaking cursors.
 *   can_leak_idle_cursors,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/refine_collection_shard_key/refine_collection_shard_key_zone_ops.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.newShardKey = {a: 1, "b.c": 1};
    $config.data.newShardKeyFields = ["a", "b.c"];
    return $config;
});
