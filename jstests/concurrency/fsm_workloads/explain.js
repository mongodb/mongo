'use strict';

/**
 * explain.js
 *
 * Runs explain() on a collection.
 *
 */
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isMongod

var $config = (function() {

    var data = {
        collNotExist: 'donotexist__',
        nInserted: 0,
        shardKey: {j: 1},
        assignEqualProbsToTransitions: function assignEqualProbsToTransitions(statesMap) {
            var states = Object.keys(statesMap);
            assertAlways.gt(states.length, 0);
            var probs = {};
            var pr = 1.0 / states.length;
            states.forEach(function(s) {
                probs[s] = pr;
            });
            return probs;
        }
    };

    function setup(db, collName, cluster) {
        assertAlways.commandWorked(db[collName].ensureIndex({j: 1}));
    }

    var states = (function() {
        function insert(db, collName) {
            db[collName].insert({i: this.nInserted, j: 2 * this.nInserted});
            this.nInserted++;
        }

        function explain(db, collName) {
            // test the three verbosity levels:
            // 'queryPlanner', 'executionStats', and 'allPlansExecution'
            ['queryPlanner', 'executionStats', 'allPlansExecution'].forEach(function(verbosity) {
                assertAlways.commandWorked(
                    db[collName].find({j: this.nInserted / 2}).explain(verbosity));
            }.bind(this));
        }

        function explainNonExistentNS(db, collName) {
            assertAlways(!db[this.collNotExist].exists());
            var res = db[this.collNotExist].find().explain();
            assertAlways.commandWorked(res);
            assertAlways(res.queryPlanner, tojson(res));
            assertAlways(res.queryPlanner.winningPlan, tojson(res));
            if (isMongod(db)) {
                assertAlways.eq(res.queryPlanner.winningPlan.stage, 'EOF', tojson(res));
            } else {
                // In the sharding case, each shard has a winningPlan
                res.queryPlanner.winningPlan.shards.forEach(function(shard) {
                    assertAlways.eq(shard.winningPlan.stage, 'EOF', tojson(res));
                });
            }
        }

        return {insert: insert, explain: explain, explainNonExistentNS: explainNonExistentNS};

    })();

    var transitions = {
        insert: {insert: 0.1, explain: 0.8, explainNonExistentNS: 0.1},
        explain: {insert: 0.7, explain: 0.2, explainNonExistentNS: 0.1},
        explainNonExistentNS: {insert: 0.4, explain: 0.5, explainNonExistentNS: 0.1}
    };

    return {
        threadCount: 10,
        iterations: 50,
        startState: 'insert',
        states: states,
        transitions: transitions,
        data: data
    };
})();
