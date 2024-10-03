/**
 * explain_count.js
 *
 * Runs explain() and count() on a collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/explain/explain.js";
import {planHasStage} from "jstests/libs/query/analyze_plan.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    function assertNCounted(num, obj, db) {
        var stage = obj.executionStats.executionStages;
        // get sharded stage(s) if counting on mongos
        if (isMongos(db)) {
            stage = stage.shards[0].executionStages;
        }
        assert.eq(num, stage.nCounted);
    }

    $config.states = Object.extend({
        explainBasicCount: function explainBasicCount(db, collName) {
            var res = db[collName].explain().count();
            assert.commandWorked(res);
            assert(planHasStage(db, res.queryPlanner.winningPlan, 'COUNT'));
        },
        explainCountHint: function explainCountHint(db, collName) {
            var res = db[collName].explain().find({i: this.nInserted / 2}).hint({i: 1}).count();
            assert.commandWorked(res);
            assert(planHasStage(db, res.queryPlanner.winningPlan, 'COUNT'));
            assert(planHasStage(db, res.queryPlanner.winningPlan, 'COUNT_SCAN'));
        },
        explainCountNoSkipLimit: function explainCountNoSkipLimit(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .find({i: this.nInserted})
                          .skip(1)
                          .count(false);
            assert.commandWorked(res);
            assertNCounted(1, res, db);
        },
        explainCountSkipLimit: function explainCountSkipLimit(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .find({i: this.nInserted})
                          .skip(1)
                          .count(true);
            assert.commandWorked(res);
            assertNCounted(0, res, db);
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
