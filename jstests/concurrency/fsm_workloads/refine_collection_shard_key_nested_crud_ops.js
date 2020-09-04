'use strict';

/**
 * Same as the base workload, but refines to a nested shard key.
 *
 * @tags: [requires_persistence, requires_sharding]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/refine_collection_shard_key_crud_ops.js');

var $config = extendWorkload($config, function($config, $super) {
    // Note the base workload assumes this is the nested key when constructing the crud ops.
    $config.data.newShardKey = {a: 1, "b.c": 1};
    $config.data.usingNestedKey = true;
    return $config;
});
