'use strict';

/**
 * findAndModify_inc_pipeline.js
 *
 * This is the same workload as findAndModify_inc.js, but substitutes a $mod-style update with a
 * pipeline-style one.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/findAndModify_inc.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.getUpdateArgument = function getUpdateArgument(fieldName) {
        return [{$addFields: {[fieldName]: {$add: ["$" + fieldName, 1]}}}];
    };

    return $config;
});
