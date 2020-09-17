'use strict';

/**
 * Same as the base workload, but refines to a nested shard key.
 *
 * @tags: [requires_persistence, requires_sharding, requires_fcv_44]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/refine_collection_shard_key_zone_ops.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.newShardKey = {a: 1, "b.c": 1};
    $config.data.newShardKeyFields = ["a", "b.c"];
    return $config;
});
