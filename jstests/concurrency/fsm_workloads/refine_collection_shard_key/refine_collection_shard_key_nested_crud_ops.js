/**
 * Same as the base workload, but refines to a nested shard key.
 *
 * @tags: [requires_persistence, requires_sharding]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/refine_collection_shard_key/refine_collection_shard_key_crud_ops.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Note the base workload assumes this is the nested key when constructing the crud ops.
    $config.data.newShardKey = {a: 1, "b.c": 1};
    $config.data.usingNestedKey = true;
    return $config;
});
