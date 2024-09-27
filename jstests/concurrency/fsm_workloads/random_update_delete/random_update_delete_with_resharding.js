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
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_base_partitioned.js";
import {
    randomUpdateDelete
} from "jstests/concurrency/fsm_workload_modifiers/random_update_delete.js";

const $partialConfig = extendWorkload($baseConfig, randomUpdateDelete);

export const $config = extendWorkload($partialConfig, function($config, $super) {
    $config.threadCount = 5;
    $config.data.partitionSize = 200;

    // reshardCollection committing may kill an ongoing operation, which will lead to partial multi
    // writes as it won't be retried.
    $config.data.expectPartialMultiWrites = true;

    // multi:true writes on sharded collections broadcast using ShardVersion::IGNORED, which can
    // cause the operation to execute on one shard before resharding commit, and on another after.
    // This can cause extra writes when a document moves shards as a result of resharding.
    $config.data.expectExtraMultiWrites = true;

    $config.states.reshardCollection = function reshardCollection(db, collName, connCache) {
        const namespace = `${db}.${collName}`;
        jsTestLog(`Attempting to reshard collection ${namespace}`);
        const result = assert.commandWorked(db.adminCommand({
            reshardCollection: namespace,
            key: this.getShardKey(collName),
            forceRedistribution: true
        }));
        jsTestLog(`Reshard collection result for ${namespace}: ${tojson(result)}`);
    };

    $config.setup = function(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        if (!TestData.runningWithShardStepdowns) {
            // Set reshardingMinimumOperationDurationMillis so that reshardCollection runs faster.
            cluster.executeOnConfigNodes((db) => {
                const res = assert.commandWorked(db.adminCommand(
                    {setParameter: 1, reshardingMinimumOperationDurationMillis: 0}));
                this.originalReshardingMinimumOperationDurationMillis = res.was;
            });
        }

        // Increase yielding so that reshardCollection more often commits in the middle of a
        // multi-write.
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 50}));
            this.internalQueryExecYieldIterationsDefault = res.was;
        });
    };

    $config.teardown = function(db, collName, cluster) {
        $super.teardown.apply(this, arguments);

        if (!TestData.runningWithShardStepdowns) {
            cluster.executeOnConfigNodes((db) => {
                const res = assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    reshardingMinimumOperationDurationMillis:
                        this.originalReshardingMinimumOperationDurationMillis
                }));
            });

            cluster.executeOnMongodNodes((db) => {
                const res = assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    internalQueryExecYieldIterations: this.internalQueryExecYieldIterationsDefault
                }));
            });
        }
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
