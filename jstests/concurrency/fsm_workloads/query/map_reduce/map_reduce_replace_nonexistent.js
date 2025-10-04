/**
 * map_reduce_replace_nonexistent.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field and stores the results in a new collection.
 *
 * Uses the "replace" action to write the results to a nonexistent
 * output collection.
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
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/map_reduce/map_reduce_inline.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Use the workload name as a prefix for the collection name,
    // since the workload name is assumed to be unique.
    $config.data.prefix = "map_reduce_replace_nonexistent";

    function uniqueCollectionName(prefix, tid) {
        return prefix + tid;
    }

    $config.states.mapReduce = function mapReduce(db, collName) {
        let outCollName = uniqueCollectionName(this.prefix, this.tid);
        // Dropping the targeted collection in case it contains garbage from previous runs and
        // creating it again to support concurrent mapReduce targeting the same collection.
        assertDropAndRecreateCollection(db, outCollName);

        let options = {
            finalize: this.finalizer,
            out: {replace: outCollName},
            query: {key: {$exists: true}, value: {$exists: true}},
        };

        let res = db[collName].mapReduce(this.mapper, this.reducer, options);
        assert.commandWorked(res);
        assert(db[outCollName].drop());
    };

    return $config;
});
