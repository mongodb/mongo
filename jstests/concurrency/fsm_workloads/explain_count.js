'use strict';

/**
 * explain_count.js
 *
 * Runs explain() and count() on a collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/explain.js');              // for $config
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongos
load('jstests/libs/analyze_plan.js');                              // for planHasStage

var $config = extendWorkload($config, function($config, $super) {

    function assertNCounted(num, obj, db) {
        var stage = obj.executionStats.executionStages;
        // get sharded stage(s) if counting on mongos
        if (isMongos(db)) {
            stage = stage.shards[0].executionStages;
        }
        assertWhenOwnColl.eq(num, stage.nCounted);
    }

    $config.states = Object.extend({
        explainBasicCount: function explainBasicCount(db, collName) {
            var res = db[collName].explain().count();
            assertAlways.commandWorked(res);
            assertAlways(planHasStage(res.queryPlanner.winningPlan, 'COUNT'));
        },
        explainCountHint: function explainCountHint(db, collName) {
            assertWhenOwnColl(function() {
                var res = db[collName].explain().find({i: this.nInserted / 2}).hint({i: 1}).count();
                assertWhenOwnColl.commandWorked(res);
                assertWhenOwnColl(planHasStage(res.queryPlanner.winningPlan, 'COUNT'));
                assertWhenOwnColl(planHasStage(res.queryPlanner.winningPlan, 'COUNT_SCAN'));
            });
        },
        explainCountNoSkipLimit: function explainCountNoSkipLimit(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .find({i: this.nInserted})
                          .skip(1)
                          .count(false);
            assertAlways.commandWorked(res);
            assertNCounted(1, res, db);
        },
        explainCountSkipLimit: function explainCountSkipLimit(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .find({i: this.nInserted})
                          .skip(1)
                          .count(true);
            assertAlways.commandWorked(res);
            assertNCounted(0, res, db);
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
