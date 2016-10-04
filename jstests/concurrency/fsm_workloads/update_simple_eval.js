'use strict';

/**
 * update_eval.js
 *
 * Creates several docs. On each iteration, each thread chooses:
 *  - a random doc
 *  - whether to $set or $unset its field
 *  - what value to $set the field to
 *  and then applies the update using db.runCommand({ eval: ... })
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');     // for extendWorkload
load('jstests/concurrency/fsm_workloads/update_simple.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.doUpdate = function doUpdate(db, collName, query, updater) {
        var evalResult = db.runCommand({
            eval: function(f, collName, query, updater) {
                return tojson(f(db, collName, query, updater));
            },
            args: [$super.data.doUpdate, collName, query, updater],
            nolock: this.nolock
        });
        assertAlways.commandWorked(evalResult);
        var res = JSON.parse(evalResult.retval);
        return res;
    };

    $config.data.nolock = false;

    return $config;
});
