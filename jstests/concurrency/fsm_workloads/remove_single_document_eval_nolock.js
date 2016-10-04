'use strict';

/**
 * remove_single_document_eval_nolock.js
 *
 * Runs remove_single_document_eval with the eval option { nolock: true }.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                   // for extendWorkload
load('jstests/concurrency/fsm_workloads/remove_single_document_eval.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.nolock = true;

    return $config;
});
