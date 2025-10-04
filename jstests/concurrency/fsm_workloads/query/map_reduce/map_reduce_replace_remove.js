/**
 * map_reduce_replace_remove.js
 *
 * Generates some random data and inserts it into a collection. Runs a map-reduce command over the
 * collection that computes the frequency counts of the 'value' field and stores the results in an
 * existing collection. Some of the random data from the source collection is removed while the
 * map-reduce operations are running to verify the cursor state is saved and restored correctly on
 * yields.
 *
 * This workload was designed to reproduce SERVER-15539.
 * @tags: [
 *   # mapReduce does not support afterClusterTime.
 *   does_not_support_causal_consistency,
 *   # Use mapReduce.
 *   requires_scripting,
 *   # Disabled because MapReduce can lose cursors if the primary goes down during the operation.
 *   does_not_support_stepdowns,
 *   # TODO (SERVER-95170): Re-enable this test in txn suites.
 *   does_not_support_transactions,
 *   # TODO (SERVER-91002): server side javascript execution is deprecated, and the balancer is not
 *   # compatible with it, once the incompatibility is taken care off we can re-enable this test
 *   assumes_balancer_off
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/map_reduce/map_reduce_replace.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.remove = function remove(db, collName) {
        for (let i = 0; i < 20; ++i) {
            let res = db[collName].remove({_id: Random.randInt(this.numDocs)}, {justOne: true});
            assert.commandWorked(res);
            assert.lte(0, res.nRemoved, tojson(res));
        }
    };

    $config.transitions = {
        init: {mapReduce: 0.5, remove: 0.5},
        mapReduce: {mapReduce: 0.5, remove: 0.5},
        remove: {mapReduce: 0.5, remove: 0.5},
    };

    return $config;
});
