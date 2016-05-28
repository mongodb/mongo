'use strict';

/**
 * explain_update.js
 *
 * Runs explain() and update() on a collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/explain.js');              // for $config
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongos

var $config = extendWorkload($config, function($config, $super) {

    $config.states = Object.extend({
        explainBasicUpdate: function explainBasicUpdate(db, collName) {
            var res =
                db[collName].explain('executionStats').update({i: this.nInserted}, {$set: {j: 49}});
            assertAlways.commandWorked(res);
            assertWhenOwnColl(function() {
                assertWhenOwnColl.eq(1, explain.executionStats.totalDocsExamined);

                // document should not have been updated.
                var doc = db[collName].findOne({i: this.nInserted});
                assertWhenOwnColl.eq(2 * this.nInserted, doc.j);
            }.bind(this));
        },
        explainUpdateUpsert: function explainUpdateUpsert(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .update({i: 2 * this.nInserted + 1},
                                  {$set: {j: 81}},
                                  /* upsert */ true);
            assertAlways.commandWorked(res);
            var stage = res.executionStats.executionStages;

            // if explaining a write command through mongos
            if (isMongos(db)) {
                stage = stage.shards[0].executionStages;
            }
            assertAlways.eq(stage.stage, 'UPDATE');
            assertWhenOwnColl(stage.wouldInsert);

            // make sure that the insert didn't actually happen.
            assertWhenOwnColl.eq(this.nInserted, db[collName].find().itcount());
        },
        explainUpdateMulti: function explainUpdateMulti(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .update({i: {$lte: 2}},
                                  {$set: {b: 3}},
                                  /* upsert */ false,
                                  /* multi */ true);
            assertAlways.commandWorked(res);
            var stage = res.executionStats.executionStages;

            // if explaining a write command through mongos
            if (isMongos(db)) {
                stage = stage.shards[0].executionStages;
            }
            assertAlways.eq(stage.stage, 'UPDATE');
            assertWhenOwnColl(!stage.wouldInsert);
            assertWhenOwnColl.eq(3, stage.nMatched);
            assertWhenOwnColl.eq(3, stage.nWouldModify);
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
