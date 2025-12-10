/**
 * Runs updateOne, deleteOne, and findAndModify without shard key against a sharded cluster while
 * there are concurrent chunk migrations.
 *
 * @tags: [
 *  requires_fcv_81,
 *  requires_sharding,
 *  uses_transactions,
 *  assumes_stable_shard_list,
 *  # TODO (SERVER-103880) Remoe this tab once getMore is supported in stepdown scenarios.
 *  requires_getmore,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {randomManualMigration} from "jstests/concurrency/fsm_workload_modifiers/random_manual_migrations.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/updateOne_without_shard_key/write_without_shard_key_base.js";
import {ConcurrentOperation} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/move_chunk_errors.js";

const $partialConfig = extendWorkload($baseConfig, randomManualMigration);
export const $config = extendWorkload($partialConfig, function ($config, $super) {
    $config.startState = "init";
    $config.transitions = {
        init: {
            moveChunk: 0.3,
            updateOne: 0.2,
            deleteOne: 0.1,
            updateOneWithId: 0.2,
            deleteOneWithId: 0.1,
            findAndModify: 0.1,
        },
        updateOne: {
            moveChunk: 0.3,
            updateOne: 0.2,
            deleteOne: 0.1,
            updateOneWithId: 0.2,
            deleteOneWithId: 0.1,
            findAndModify: 0.1,
        },
        deleteOne: {
            moveChunk: 0.3,
            updateOne: 0.3,
            deleteOne: 0.2,
            updateOneWithId: 0.3,
            deleteOneWithId: 0.2,
            findAndModify: 0.2,
        },
        updateOneWithId: {
            moveChunk: 0.3,
            updateOne: 0.2,
            deleteOne: 0.1,
            updateOneWithId: 0.2,
            deleteOneWithId: 0.1,
            findAndModify: 0.1,
        },
        deleteOneWithId: {
            moveChunk: 0.3,
            updateOne: 0.3,
            deleteOne: 0.2,
            updateOneWithId: 0.3,
            deleteOneWithId: 0.2,
            findAndModify: 0.2,
        },
        findAndModify: {
            moveChunk: 0.3,
            updateOne: 0.2,
            deleteOne: 0.1,
            updateOneWithId: 0.2,
            deleteOneWithId: 0.1,
            findAndModify: 0.1,
        },
        moveChunk: {
            moveChunk: 0.3,
            updateOne: 0.2,
            deleteOne: 0.1,
            updateOneWithId: 0.2,
            deleteOneWithId: 0.1,
            findAndModify: 0.1,
        },
    };

    $config.data.getConcurrentOperations = () => {
        return [
            ...$super.data.getConcurrentOperations(),
            ConcurrentOperation.BroadcastWrite,
            ConcurrentOperation.ShardKeyUpdate,
        ];
    };

    return $config;
});
