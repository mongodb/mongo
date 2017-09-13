'use strict';

/**
 * update_simple_noindex.js
 *
 * Executes the update_simple.js workload after dropping all non-_id indexes on
 * the collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                 // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_simple.js');              // for $config
load('jstests/concurrency/fsm_workload_modifiers/drop_all_indexes.js');  // for dropAllIndexes

var $config = extendWorkload($config, dropAllIndexes);
