/**
 * map_reduce_with_chunk_migrations.js
 *
 * This tests exercises mapReduce on a collection during chunk migrations. If extending this
 * workload, consider overriding the following:
 *
 * $config.data.collWithMigrations: collection to run chunk migrations against (default is the
 * input collection of the mapReduce).
 * $config.state.mapReduce: function to execute the mapReduce.
 *
 * @tags: [
 *  requires_sharding, assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  # mapReduce does not support afterClusterTime.
 *  does_not_support_causal_consistency,
 *  # Use mapReduce.
 *  requires_scripting,
 *  # Disabled because MapReduce can lose cursors if the primary goes down during the operation.
 *  does_not_support_stepdowns,
 *  # TODO (SERVER-95170): Re-enable this test in txn suites.
 *  does_not_support_transactions,
 *  # TODO (SERVER-91002): server side javascript execution is deprecated, and the balancer is not
 *  # compatible with it, once the incompatibility is taken care off we can re-enable this test
 *  assumes_balancer_off
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_moveChunk_partitioned.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // The base setup will insert 'partitionSize' number of documents per thread, evenly
    // distributing across the chunks. Documents will only have the "_id" field.
    $config.data.partitionSize = 50;
    $config.threadCount = 2;
    $config.iterations = 100;
    $config.data.numDocs = $config.data.partitionSize * $config.threadCount;

    // By default, the collection that will be sharded with concurrent chunk migrations will be the
    // one that the aggregate is run against.
    $config.data.collWithMigrations = $config.collName;

    $config.transitions = {
        init: {mapReduce: 1},
        mapReduce: {
            moveChunk: 0.2,
            mapReduce: 0.8,
        },
        moveChunk: {mapReduce: 1},
    };

    /**
     * Moves a random chunk in the target collection.
     */
    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        $super.states.moveChunk.apply(this, [db, this.collWithMigrations, connCache]);
    };

    /**
     * Executes a mapReduce with output mode "replace".
     */
    $config.states.mapReduce = function mapReduce(db, collName, connCache) {
        const map = function () {
            emit(this._id, 1);
        };
        const reduce = function (k, values) {
            return Array.sum(values);
        };

        const res = db[collName].mapReduce(map, reduce, {out: {replace: this.resultsCollection}});
        assert.commandWorked(res);

        assert.eq(this.numDocs, db[this.resultsCollection].find().itcount());
    };

    /**
     * Uses the base class init() to initialize this thread for both collections.
     */
    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, [db, collName, connCache]);

        // Init the target collection in a similar manner, if it is different than the default
        // collection.
        if (collName != this.collWithMigrations) {
            $super.states.init.apply(this, [db, this.collWithMigrations, connCache]);
        }

        // Use a unique target collection name per thread to avoid colliding during the final rename
        // of the mapReduce.
        this.resultsCollection = "map_reduce_with_chunk_migrations_out_" + this.tid;
    };

    /**
     * Initializes the aggregate collection and the target collection for chunk migrations as
     * sharded with an even distribution across each thread ID.
     */
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, [db, collName, cluster]);

        if (collName != this.collWithMigrations) {
            // Setup the target collection in a similar manner. Note that the FSM infrastructure
            // will have already enabled sharded on collName, but we need to manually do it for the
            // output collection.
            cluster.shardCollection(db[this.collWithMigrations], this.shardKey, false);
            $super.setup.apply(this, [db, this.collWithMigrations, cluster]);
        }
    };

    return $config;
});
