'use strict';

/**
 * This test performs random updates and deletes (see random_update_delete.js) on a sharded
 * collection while performing random reshardCollections.
 *
 * @tags: [
 * requires_sharding,
 * requires_fcv_80,
 * assumes_balancer_off,
 * ];
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/sharded_base_partitioned.js";
import {
    randomUpdateDelete
} from "jstests/concurrency/fsm_workload_modifiers/random_update_delete.js";

const $partialConfig = extendWorkload($baseConfig, randomUpdateDelete);

export const $config = extendWorkload($partialConfig, function($config, $super) {
    $config.threadCount = 5;
    $config.data.partitionSize = 100;

    // reshardCollection committing may kill an ongoing operation (which would lead to partial multi
    // writes), but the operation will be retried, leading to extra multi writes.
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

    // TODO SERVER-91634: Set weight for reshardCollection to 0.2.
    const weights = {reshardCollection: 0.0, performUpdates: 0.4, performDeletes: 0.4};
    $config.transitions = {
        init: weights,
        reshardCollection: weights,
        performUpdates: weights,
        performDeletes: weights,
    };

    return $config;
});
