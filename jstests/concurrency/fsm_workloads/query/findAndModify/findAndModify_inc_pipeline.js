/**
 * findAndModify_inc_pipeline.js
 *
 * This is the same workload as findAndModify_inc.js, but substitutes a $mod-style update with a
 * pipeline-style one.
 *
 * This workload implicitly assumes that its tid ranges are [0, $config.threadCount). This
 * isn't guaranteed to be true when they are run in parallel with other workloads. Therefore
 * it can't be run in concurrency simultaneous suites.
 * @tags: [incompatible_with_concurrency_simultaneous]
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
