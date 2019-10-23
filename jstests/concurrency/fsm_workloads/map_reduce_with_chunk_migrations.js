'use strict';

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
 *  assumes_autosplit_off,
 *  requires_non_retryable_writes,
 *  # mapReduce does not support afterClusterTime.
 *  does_not_support_causal_consistency
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                     // for extendWorkload
load('jstests/concurrency/fsm_workloads/sharded_moveChunk_partitioned.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    // The base setup will insert 'partitionSize' number of documents per thread, evenly
    // distributing across the chunks. Documents will only have the "_id" field.
    $config.data.partitionSize = 50;
    $config.threadCount = 2;
    $config.iterations = 100;
    $config.data.numDocs = $config.data.partitionSize * $config.threadCount;

    // By default, the collection that will be sharded with concurrent chunk migrations will be the
    // one that the aggregate is run against.
    $config.data.collWithMigrations = $config.collName;
    $config.data.resultsCollection = "map_reduce_with_chunk_migrations_out";

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
        const map = function() {
            emit(this._id, 1);
        };
        const reduce = function(k, values) {
            return Array.sum(values);
        };

        const res = db[collName].mapReduce(map, reduce, {out: {replace: this.resultsCollection}});
        assertWhenOwnColl.commandWorked(res);

        // TODO SERVER-43290 Support for cluster stats should be able to enable this check.
        // assertWhenOwnColl.eq(
        //     this.numDocs, res.counts.output, `Expected each _id to be output once:
        //     ${tojson(res)}`);
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

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: true}));
    };

    return $config;
});
