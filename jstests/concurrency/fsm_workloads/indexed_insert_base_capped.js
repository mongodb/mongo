/**
 * indexed_insert_base_capped.js
 *
 * Executes the indexed_insert_base.js workload on a capped collection.
 * @tags: [requires_capped]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/indexed_insert_base.js";
load('jstests/concurrency/fsm_workload_modifiers/make_capped.js');  // for makeCapped

export const $config = extendWorkload($baseConfig, makeCapped);

// Remove the shard key for capped collections, as they cannot be sharded
delete $config.data.shardKey;
