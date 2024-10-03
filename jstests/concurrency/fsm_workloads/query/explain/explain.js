/**
 * explain.js
 *
 * Runs explain() on a collection.
 *
 */
import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {getWinningPlan} from "jstests/libs/query/analyze_plan.js";

export const $config = (function() {
    var data = {
        collNotExist: 'donotexist__',
        nInserted: 0,
        shardKey: {j: 1},
        assignEqualProbsToTransitions: function assignEqualProbsToTransitions(statesMap) {
            var states = Object.keys(statesMap);
            assert.gt(states.length, 0);
            var probs = {};
            var pr = 1.0 / states.length;
            states.forEach(function(s) {
                probs[s] = pr;
            });
            return probs;
        }
    };

    var states = (function() {
        function insert(db, collName) {
            db[collName].insert({i: this.nInserted, j: 2 * this.nInserted});
            this.nInserted++;
        }

        function explain(db, collName) {
            // test the three verbosity levels:
            // 'queryPlanner', 'executionStats', and 'allPlansExecution'
            ['queryPlanner', 'executionStats', 'allPlansExecution'].forEach(function(verbosity) {
                assert.commandWorked(db[collName].find({j: this.nInserted / 2}).explain(verbosity));
            }.bind(this));
        }

        function explainNonExistentNS(db, collName) {
            assert(!db[this.collNotExist].exists());
            var res = db[this.collNotExist].find().explain();
            assert.commandWorked(res);
            assert(res.queryPlanner, tojson(res));
            assert(res.queryPlanner.winningPlan, tojson(res));
            if (isMongod(db) && !TestData.testingReplicaSetEndpoint) {
                assert.eq(getWinningPlan(res.queryPlanner).stage, 'EOF', tojson(res));
            } else {
                // In the sharding case, each shard has a winningPlan
                res.queryPlanner.winningPlan.shards.forEach(function(shard) {
                    assert.eq(getWinningPlan(shard).stage, 'EOF', tojson(res));
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
