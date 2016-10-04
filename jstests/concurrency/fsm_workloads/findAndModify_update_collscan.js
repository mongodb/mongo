'use strict';

/**
 * findAndModify_update_collscan.js
 *
 * Each thread inserts multiple documents into a collection, and then
 * repeatedly performs the findAndModify command. A single document is
 * selected based on 'query' and 'sort' specifications, and updated
 * using either the $min or $max operator.
 *
 * Attempts to force a collection scan by not creating an index.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/findAndModify_update.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    // Do not create the { tid: 1, value: 1 } index so that a
    // collection
    // scan is performed for the query and sort operations.
    $config.setup = function setup(db, collName, cluster) {};

    // Remove the shardKey so that a collection scan is performed
    delete $config.data.shardKey;

    return $config;
});
