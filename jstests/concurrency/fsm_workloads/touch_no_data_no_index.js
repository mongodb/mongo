'use strict';

/**
 * touch_no_data_no_index.js
 *
 * Bulk inserts documents in batches of 100, uses touch as a no-op,
 * and queries to verify the number of documents inserted by the thread.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/touch_base.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.generateTouchCmdObj = function generateTouchCmdObj(collName) {
        return {touch: collName, data: false, index: false};
    };

    $config.states.touch = function touch(db, collName) {
        var res = db.runCommand(this.generateTouchCmdObj(collName));
        // The command always fails because "index" and "data" are both false
        assertAlways.commandFailed(res);
    };

    return $config;
});
