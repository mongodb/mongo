'use strict';

/**
 * indexed_insert_large_noindex.js
 *
 * Executes the indexed_insert_large.js workload after dropping its index.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_large.js');      // for $config
load('jstests/concurrency/fsm_workload_modifiers/indexed_noindex.js');  // for indexedNoindex

var $config = extendWorkload($config, indexedNoindex);
