'use strict';

/**
 * update_inc_capped.js
 *
 * Executes the update_inc.js workload on a capped collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_inc.js');            // for $config
load('jstests/concurrency/fsm_workload_modifiers/make_capped.js');  // for makeCapped

var $config = extendWorkload($config, makeCapped);
