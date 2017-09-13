'use strict';

/**
 * remove_single_document_eval.js
 *
 * Runs remove_single_document using the eval command.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');              // for extendWorkload
load('jstests/concurrency/fsm_workloads/remove_single_document.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.doRemove = function doRemove(db, collName, query, options) {
        var evalResult = db.runCommand({
            eval: function(f, collName, query, options) {
                return tojson(f(db, collName, query, options));
            },
            args: [$super.data.doRemove, collName, query, options],
            nolock: this.nolock
        });
        assertAlways.commandWorked(evalResult);
        var res = JSON.parse(evalResult.retval);
        return res;
    };

    $config.data.assertResult = function assertResult(res) {
        assertWhenOwnColl.eq(1, res.nRemoved, tojson(res));
    };

    $config.data.nolock = false;

    // scale down threadCount and iterations because eval takes a global lock
    $config.threadCount = 5;
    $config.iterations = 10;

    return $config;
});
