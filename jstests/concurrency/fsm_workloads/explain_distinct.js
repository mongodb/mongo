'use strict';

/**
 * explain_distinct.js
 *
 * Runs explain() and distinct() on a collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/explain.js');     // for $config
load('jstests/libs/analyze_plan.js');                     // for planHasStage

var $config = extendWorkload($config, function($config, $super) {
    $config.states = Object.extend({
        explainBasicDistinct: function(db, collName) {
            var res = db[collName].explain().distinct('i');
            assertAlways.commandWorked(res);
            assertAlways(planHasStage(res.queryPlanner.winningPlan, 'COLLSCAN'));
        },
        explainDistinctIndex: function(db, collName) {
            var res = db[collName].explain().distinct('_id');
            assertAlways.commandWorked(res);
            assertAlways(planHasStage(res.queryPlanner.winningPlan, 'PROJECTION'));
            assertAlways(planHasStage(res.queryPlanner.winningPlan, 'DISTINCT_SCAN'));
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
