'use strict';

/**
 * update_inc_pipeline.js
 *
 * This is the same workload as update_inc.js, but substitutes a $mod-style update with a
 * pipeline-style one.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_inc.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.getUpdateArgument = function getUpdateArgument(fieldName) {
        return [{$set: {[fieldName]: {$add: ["$" + fieldName, 1]}}}];
    };

    $config.data.update_inc = "update_inc_pipeline";

    return $config;
});
