'use strict';

/**
 * explain_remove.js
 *
 * Runs explain() and remove() on a collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/explain.js');     // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.states = Object.extend({
        explainSingleRemove: function explainSingleRemove(db, collName) {
            var res = db[collName]
                          .explain('executionStats')
                          .remove({i: this.nInserted}, /* justOne */ true);
            assertAlways.commandWorked(res);
            assertWhenOwnColl(function() {
                assertWhenOwnColl.eq(1, res.executionStats.totalDocsExamined);

                // the document should not have been deleted.
                assertWhenOwnColl.eq(1, db[collName].find({i: this.nInserted}).itcount());
            }.bind(this));
        },
        explainMultiRemove: function explainMultiRemove(db, collName) {
            var res =
                db[collName].explain('executionStats').remove({i: {$lte: this.nInserted / 2}});
            assertAlways.commandWorked(res);
            assertWhenOwnColl(function() {
                assertWhenOwnColl.eq(this.nInserted / 2 + 1,
                                     explain.executionStats.totalDocsExamined);
                // no documents should have been deleted
                assertWhenOwnColl.eq(this.nInserted, db[collName].itcount());
            }.bind(this));
        }
    },
                                   $super.states);

    $config.transitions = Object.extend(
        {explain: $config.data.assignEqualProbsToTransitions($config.states)}, $super.transitions);

    return $config;
});
