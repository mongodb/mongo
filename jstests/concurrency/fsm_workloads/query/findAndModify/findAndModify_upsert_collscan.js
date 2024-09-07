/**
 * findAndModify_upsert_collscan.js
 *
 * Each thread repeatedly performs the findAndModify command, specifying
 * upsert as either true or false. A single document is selected (or
 * created) based on the 'query' specification, and updated using the
 * $push operator.
 *
 * Forces 'sort' to perform a collection scan by using $natural.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/findAndModify/findAndModify_upsert.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.sort = {$natural: 1};

    return $config;
});
