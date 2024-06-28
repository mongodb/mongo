'use strict';

/**
 * This test performs random updates and deletes (see random_update_delete.js) on an unsharded
 * collection while performing random moveCollections.
 *
 * @tags: [
 * requires_sharding,
 * requires_fcv_80,
 * assumes_balancer_off,
 * ];
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    randomUpdateDelete
} from "jstests/concurrency/fsm_workload_modifiers/random_update_delete.js";

const $baseConfig = {
    threadCount: 5,
    iterations: 50,
    passConnectionCache: true,
    startState: 'init',
    states: {
        init: function init(db, collName, connCache) {},
    },
    transitions: {init: {init: 1}},
    setup: function setup(db, collName, cluster) {
        // The runner will implicitly shard the collection if we are in a sharded cluster, so
        // unshard it.
        assert.commandWorked(db.adminCommand({unshardCollection: `${db}.${collName}`}));
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.threadCount * 100; ++i) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());
    },
};

const $partialConfig = extendWorkload($baseConfig, randomUpdateDelete);

export const $config = extendWorkload($partialConfig, function($config, $super) {
    // moveCollection committing may kill an ongoing operation (leading to partial multi writes),
    // but we should never retry (which would lead to extra multi writes). This matches the possible
    // behavior for collections in a plain replica set.
    $config.data.expectPartialMultiWrites = true;
    $config.data.moveCollectionIterations = $config.iterations / 10;

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.moveCollectionsPerformed = 0;
    };

    $config.states.moveCollection = function moveCollection(db, collName, connCache) {
        if (this.tid !== 0) {
            // Only run moveCollection from a single thread to avoid ConflictingOperationInProgress
            // errors from two threads trying to run moveCollection with different arguments at the
            // same time.
            return;
        }

        if (this.moveCollectionsPerformed > this.moveCollectionIterations) {
            // moveCollection is much slower than update/delete, so limit the number of executions
            // so we don't end up in a situation where this thread is running moveCollections long
            // after the other threads have already finished.
            return;
        }

        const shardNames = Object.keys(connCache.shards);
        const toShard = shardNames[Random.randInt(shardNames.length)];
        const namespace = `${db}.${collName}`;
        jsTestLog(`Attempting to move collection ${namespace} to shard ${toShard}`);
        const result = assert.commandWorked(db.adminCommand({moveCollection: namespace, toShard}));
        jsTestLog(`Move collection result for ${namespace} to shard ${toShard}: ${tojson(result)}`);
        this.moveCollectionsPerformed++;
    };

    // TODO SERVER-91634: Set weight for moveCollection to 0.2.
    const weights = {moveCollection: 0.0, performUpdates: 0.4, performDeletes: 0.4};
    $config.transitions = {
        init: weights,
        moveCollection: weights,
        performUpdates: weights,
        performDeletes: weights,
    };

    return $config;
});
