'use strict';

/**
 * agg_with_chunk_migrations.js
 *
 * This tests exercises aggregations on a collection during chunk migrations. If extending this
 * workload, consider overriding the following:
 *
 * $config.data.collWithMigrations: collection to run chunk migrations against (default is the
 * collection of the aggregation itself).
 * $config.state.aggregate: function to execute the aggregation.
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off,
 * requires_non_retryable_writes]
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

    $config.transitions = {
        init: {aggregate: 1},
        aggregate: {
            moveChunk: 0.2,
            aggregate: 0.8,
        },
        moveChunk: {aggregate: 1},
    };

    /**
     * Moves a random chunk in the target collection.
     */
    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        $super.states.moveChunk.apply(this, [db, this.collWithMigrations, connCache]);
    };

    /**
     * Default behavior executes an aggregation with an empty pipeline.
     */
    $config.states.aggregate = function aggregate(db, collName, connCache) {
        const res = db[collName].aggregate([]);
        assert.eq(this.numDocs, res.itcount());
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
    };

    return $config;
});
