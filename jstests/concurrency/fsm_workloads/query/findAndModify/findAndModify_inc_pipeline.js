/**
 * findAndModify_inc_pipeline.js
 *
 * This is the same workload as findAndModify_inc.js, but substitutes a $mod-style update with a
 * pipeline-style one.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/findAndModify/findAndModify_inc.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.getUpdateArgument = function getUpdateArgument(fieldName) {
        return [{$addFields: {[fieldName]: {$add: ["$" + fieldName, 1]}}}];
    };

    return $config;
});
