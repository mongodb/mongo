/**
 * explain_remove.js
 *
 * Runs explain() and remove() on a collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/explain/explain.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states = Object.extend({
        explainSingleRemove: function explainSingleRemove(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .remove({i: this.nInserted}, /* justOne */ true);
            assert.commandWorked(res);
            assert.eq(1, res.executionStats.totalDocsExamined);

            // the document should not have been deleted.
            assert.eq(1, db[collName].find({i: this.nInserted}).itcount());
        },
        explainMultiRemove: function explainMultiRemove(db, collName) {
            var res =
                db[collName].explain('executionStats').remove({i: {$lte: this.nInserted / 2}});
            assert.commandWorked(res);
            assert.eq(this.nInserted / 2 + 1,
                      explain.executionStats.totalDocsExamined);  // eslint-disable-line
            // no documents should have been deleted
            assert.eq(this.nInserted, db[collName].itcount());
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
