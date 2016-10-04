'use strict';

/**
 * touch_data.js
 *
 * Bulk inserts documents in batches of 100, uses touch on "data" but not "index",
 * and queries to verify the number of documents inserted by the thread.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/touch_base.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.generateTouchCmdObj = function generateTouchCmdObj(collName) {
        return {touch: collName, data: true, index: false};
    };

    return $config;
});
