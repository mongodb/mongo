/**
 * map_reduce_merge.js
 *
 * Generates some random data and inserts it into a collection. Runs a
 * map-reduce command over the collection that computes the frequency
 * counts of the 'value' field and stores the results in an existing
 * collection on a separate database.
 *
 * Uses the "merge" action to combine the results with the contents
 * of the output collection.
 *
 * Writes the results of each thread to the same collection.
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

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Use the workload name as the database name,
    // since the workload name is assumed to be unique.
    let uniqueDBName = "map_reduce_merge";

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        this.outDBName = db.getName() + uniqueDBName;
    };

    $config.states.mapReduce = function mapReduce(db, collName) {
        let outDB = db.getSiblingDB(this.outDBName);
        let fullName = outDB[collName].getFullName();
        assert(outDB[collName].exists() !== null, "output collection '" + fullName + "' should exist");

        // Have all threads combine their results into the same collection
        let options = {finalize: this.finalizer, out: {merge: collName, db: this.outDBName}};

        let res = db[collName].mapReduce(this.mapper, this.reducer, options);
        assert.commandWorked(res);
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        let outDB = db.getSiblingDB(db.getName() + uniqueDBName);
        assert.commandWorked(outDB.createCollection(collName));
    };

    return $config;
});
