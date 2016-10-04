'use strict';

/**
 * indexed_insert_base_capped.js
 *
 * Executes the indexed_insert_base.js workload on a capped collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js');   // for $config
load('jstests/concurrency/fsm_workload_modifiers/make_capped.js');  // for makeCapped

var $config = extendWorkload($config, makeCapped);

// Remove the shard key for capped collections, as they cannot be sharded
delete $config.data.shardKey;
