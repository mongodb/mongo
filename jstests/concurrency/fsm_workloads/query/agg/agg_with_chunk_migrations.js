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
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_on,
 *  requires_non_retryable_writes,
 *  requires_getmore,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {randomManualMigration} from "jstests/concurrency/fsm_workload_modifiers/random_manual_migrations.js";

const $baseConfig = (function () {
    let data = {
        numDocs: 100,
        shardKey: {_id: 1},
        shardKeyField: "_id",
        // By default, the collection that will be sharded with concurrent chunk migrations will be
        // the one that the aggregate is run against.
        collWithMigrations: undefined,
    };

    let states = {
        init: function init(db, collName, connCache) {},
        aggregate: function aggregate(db, collName, connCache) {
            const res = db[collName].aggregate([]);
            assert.eq(this.numDocs, res.itcount());
        },
        // Will be overridden by the extendWorkload below, included just so that our transitions are
        // correct.
        moveChunk: function (db, collName, connCache) {},
    };

    let transitions = {
        init: {aggregate: 1},
        aggregate: {
            moveChunk: 0.2,
            aggregate: 0.8,
        },
        moveChunk: {aggregate: 1},
    };

    let setup = function setup(db, collName, cluster) {
        let bulk = db[collName].initializeUnorderedBulkOp();
        for (let index = 0; index < this.numDocs; index++) {
            bulk.insert({_id: index});
        }
        assert.commandWorked(bulk.execute());
        if (this.collWithMigrations && collName != this.collWithMigrations) {
            // Setup the target collection in a similar manner. Note that the FSM infrastructure
            // will have already enabled sharded on collName, but we need to manually do it for the
            // output collection.
            cluster.shardCollection(db[this.collWithMigrations], this.shardKey, false);
            let bulk = db[this.collWithMigrations].initializeUnorderedBulkOp();
            for (let index = 0; index < this.numDocs; index++) {
                bulk.insert({_id: index});
            }
            assert.commandWorked(bulk.execute());
        }
    };

    return {
        threadCount: 2,
        iterations: 50,
        startState: "init",
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
        passConnectionCache: true,
    };
})();

export const $config = extendWorkload($baseConfig, randomManualMigration);
