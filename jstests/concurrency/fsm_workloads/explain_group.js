'use strict';

/**
 * explain_group.js
 *
 * Runs explain() and group() on a collection.
 *
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/explain.js');     // for $config
load('jstests/libs/analyze_plan.js');                     // for planHasStage

var $config = extendWorkload($config, function($config, $super) {

    $config.states = Object.extend({
        explainBasicGroup: function explainBasicGroup(db, collName) {
            var res =
                db[collName].explain().group({key: {i: 1}, initial: {}, reduce: function() {}});
            assertAlways.commandWorked(res);
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
