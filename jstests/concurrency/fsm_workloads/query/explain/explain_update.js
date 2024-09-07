/**
 * explain_update.js
 *
 * Runs explain() and update() on a collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/explain/explain.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states = Object.extend({
        explainBasicUpdate: function explainBasicUpdate(db, collName) {
            var res =
                db[collName].explain('executionStats').update({i: this.nInserted}, {$set: {j: 49}});
            assert.commandWorked(res);
            // eslint-disable-next-line
            assert.eq(1, explain.executionStats.totalDocsExamined);

            // document should not have been updated.
            var doc = db[collName].findOne({i: this.nInserted});
            assert.eq(2 * this.nInserted, doc.j);
        },
        explainUpdateUpsert: function explainUpdateUpsert(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .update({i: 2 * this.nInserted + 1},
                                  {$set: {j: 81}},
                                  /* upsert */ true);
            assert.commandWorked(res);
            var stage = res.executionStats.executionStages;

            // if explaining a write command through mongos
            if (isMongos(db)) {
                stage = stage.shards[0].executionStages;
            }
            assert.eq(stage.stage, 'UPDATE');
            assert(stage.nWouldUpsert == 1);

            // make sure that the insert didn't actually happen.
            assert.eq(this.nInserted, db[collName].find().itcount());
        },
        explainUpdateMulti: function explainUpdateMulti(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .update({i: {$lte: 2}},
                                  {$set: {b: 3}},
                                  /* upsert */ false,
                                  /* multi */ true);
            assert.commandWorked(res);
            var stage = res.executionStats.executionStages;

            // if explaining a write command through mongos
            if (isMongos(db)) {
                stage = stage.shards[0].executionStages;
            }
            assert.eq(stage.stage, 'UPDATE');
            assert(stage.nWouldUpsert == 0);
            assert.eq(3, stage.nMatched);
            assert.eq(3, stage.nWouldModify);
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
