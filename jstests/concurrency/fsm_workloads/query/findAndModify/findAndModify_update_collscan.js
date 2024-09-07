/**
 * findAndModify_update_collscan.js
 *
 * Each thread inserts multiple documents into a collection, and then
 * repeatedly performs the findAndModify command. A single document is
 * selected based on 'query' and 'sort' specifications, and updated
 * using either the $min or $max operator.
 *
 * Attempts to force a collection scan by not creating an index.
 *
 * @tags: [
 *   # PM-1632 was delivered in 7.1.
 *   requires_fcv_71
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/findAndModify/findAndModify_update.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Do not create the { tid: 1, value: 1 } index so that a
    // collection
    // scan is performed for the query and sort operations.
    $config.setup = function setup(db, collName, cluster) {};

    // Remove the shardKey so that a collection scan is performed
    delete $config.data.shardKey;

    return $config;
});
