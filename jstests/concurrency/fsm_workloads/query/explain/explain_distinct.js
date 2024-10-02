/**
 * explain_distinct.js
 *
 * Runs explain() and distinct() on a collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/explain/explain.js";
import {planHasStage} from "jstests/libs/query/analyze_plan.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states = Object.extend({
        explainBasicDistinct: function(db, collName) {
            var res = db[collName].explain().distinct('i');
            assert.commandWorked(res);
            assert(planHasStage(db, res.queryPlanner.winningPlan, 'COLLSCAN'));
        },
        explainDistinctIndex: function(db, collName) {
            var res = db[collName].explain().distinct('_id');
            assert.commandWorked(res);
            assert(planHasStage(db, res.queryPlanner.winningPlan, 'PROJECTION'));
            assert(planHasStage(db, res.queryPlanner.winningPlan, 'DISTINCT_SCAN'));
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
