/**
 * explain_aggregate.js
 *
 * Runs explain() and aggregate() on a collection.
 *
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/explain.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    function assertCursorStages(num, obj) {
        assertAlways(obj.stages, tojson(obj));
        assertAlways.eq(num, obj.stages.length, tojson(obj.stages));
        assertAlways(obj.stages[0].$cursor, tojson(obj.stages[0]));
        assertAlways(obj.stages[0].$cursor.hasOwnProperty('queryPlanner'),
                     tojson(obj.stages[0].$cursor));
    }

    $config.states = Object.extend({
        explainMatch: function explainMatch(db, collName) {
            var res = db[collName].explain().aggregate([{$match: {i: this.nInserted / 2}}]);
            assertAlways.commandWorked(res);

            // stages reported: $cursor
            assertCursorStages(1, res);
        },
        explainMatchProject: function explainMatchProject(db, collName) {
            var res = db[collName].explain().aggregate(
                [{$match: {i: this.nInserted / 3}}, {$project: {i: 1}}]);
            assertAlways.commandWorked(res);

            // stages reported: $cursor, $project
            assertCursorStages(2, res);
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
