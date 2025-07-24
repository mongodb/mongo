'use strict';

/**
 * This test performs random updates and deletes (see random_update_delete.js) on a sharded
 * collection while performing random reshardCollections.
 *
 * @tags: [
 * requires_sharding,
 * requires_fcv_80,
 * assumes_balancer_off,
 * # Changes server parameters.
 * incompatible_with_concurrency_simultaneous,
 * # TODO (SERVER-91251): Run this with stepdowns on sanitizers.
 * tsan_incompatible,
 * incompatible_aubsan,
 * ];
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    randomUpdateDelete
} from "jstests/concurrency/fsm_workload_modifiers/random_update_delete.js";

const $baseConfig = {
    threadCount: 5,
    iterations: 10,
    passConnectionCache: true,
    startState: 'init',
    states: {
        init: function init(db, collName, connCache) {},
    },
    transitions: {init: {init: 1}},
    setup: function(db, collName, cluster) {
        if (!TestData.runningWithShardStepdowns) {
            // Set reshardingMinimumOperationDurationMillis so that reshardCollection runs faster.
            cluster.executeOnConfigNodes((db) => {
                const res = assert.commandWorked(db.adminCommand(
                    {setParameter: 1, reshardingMinimumOperationDurationMillis: 0}));
                this.originalReshardingMinimumOperationDurationMillis = res.was;
            });
        }
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.threadCount * 200; ++i) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());
    },

    teardown: function(db, collName, cluster) {
        if (!TestData.runningWithShardStepdowns) {
            cluster.executeOnConfigNodes((db) => {
                const res = assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    reshardingMinimumOperationDurationMillis:
                        this.originalReshardingMinimumOperationDurationMillis
                }));
            });
        }
    },
};

const $partialConfig = extendWorkload($baseConfig, randomUpdateDelete);

export const $config = extendWorkload($partialConfig, function($config, $super) {
    // reshardCollection committing may kill an ongoing operation, which will lead to partial
    // multi writes as it won't be retried.
    $config.data.expectPartialMultiWrites = true;
    // multi:true writes on sharded collections broadcast using ShardVersion::IGNORED, which
    // can cause the operation to execute on one shard before resharding commit, and on
    // another after. This can cause extra writes when a document moves shards as a result
    // of resharding.
    $config.data.expectExtraMultiWrites = true;

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
    };

    $config.states.reshardCollection = function reshardCollection(db, collName, connCache) {
        const namespace = `${db}.${collName}`;
        jsTestLog(`Attempting to reshard collection ${namespace}`);
        const result = assert.commandWorked(db.adminCommand({
            reshardCollection: namespace,
            key: this.getShardKey(collName),
            numInitialChunks: 1,
            forceRedistribution: true
        }));
        jsTestLog(`Reshard collection result for ${namespace}: ${tojson(result)}`);
    };

    const weights = {reshardCollection: 0.1, performUpdates: 0.45, performDeletes: 0.45};
    $config.transitions = {
        init: weights,
        reshardCollection: weights,
        performUpdates: weights,
        performDeletes: weights,
    };

    return $config;
});
