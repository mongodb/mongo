'use strict';

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
load('jstests/concurrency/fsm_libs/extend_workload.js');            // for extendWorkload
load('jstests/concurrency/fsm_workloads/findAndModify_upsert.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.sort = {$natural: 1};

    return $config;
});
