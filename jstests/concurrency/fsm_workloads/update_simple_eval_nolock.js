'use strict';

/**
 * update_simple_eval_nolock.js
 *
 * Runs update_simple_eval with the eval option { nolock: true }.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');          // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_simple_eval.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.nolock = true;

    return $config;
});
