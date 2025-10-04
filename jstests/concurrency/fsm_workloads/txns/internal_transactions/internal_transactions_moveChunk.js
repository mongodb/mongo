/**
 * Runs insert, update, delete and findAndModify commands against a sharded collection inside
 * single-shard and cross-shard internal transactions using all the available client session
 * settings, and occasionally runs chunk migrations. Only runs on sharded clusters.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 *  uses_transactions,
 *  antithesis_incompatible,
 *  # startCommit times out
 *  does_not_support_config_fuzzer,
 *  assumes_stable_shard_list,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {randomManualMigration} from "jstests/concurrency/fsm_workload_modifiers/random_manual_migrations.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/txns/internal_transactions/internal_transactions_sharded.js";

const $partialConfig = extendWorkload($baseConfig, randomManualMigration);
export const $config = extendWorkload($partialConfig, function ($config, $super) {
    $config.transitions = {
        init: {
            moveChunk: 0.2,
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
        },
        moveChunk: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2,
        },
        internalTransactionForInsert: {
            moveChunk: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2,
        },
        internalTransactionForUpdate: {
            moveChunk: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2,
        },
        internalTransactionForDelete: {
            moveChunk: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2,
        },
        internalTransactionForFindAndModify: {
            moveChunk: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2,
        },
        verifyDocuments: {
            moveChunk: 0.2,
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
        },
    };

    return $config;
});
