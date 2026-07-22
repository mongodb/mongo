/**
 * Concurrently performs CRUD operations, DDL commands and FCV changes and verifies guarantees are
 * not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   does_not_support_causal_consistency,
 *   # The mutex mechanism used in CRUD and drop states does not support stepdown
 *   does_not_support_stepdowns,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # Relies on internalInsertMaxBatchSize to be 64 or above, but it may be fuzzed to lower values.
 *   does_not_support_config_fuzzer,
 *   runs_set_fcv,
 *   # TODO SERVER-130943: The add_remove_shards hook does not handle concurrent setFCV executions.
 *   assumes_stable_shard_list,
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/random_ddl_crud_operations.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    const kReshardingSetFcvErrors = [
        ErrorCodes.CommandNotSupported,
        ErrorCodes.ReshardCollectionInterruptedDueToFCVChange,
    ];

    $config.data.kReshardingAcceptableErrors =
        $config.data.kReshardingAcceptableErrors.concat(kReshardingSetFcvErrors);

    $config.states.setFCV = function (db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog("setFCV to " + targetFCV);
        try {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
            );
        } catch (e) {
            if (handleRandomSetFCVErrors(e, targetFCV)) return;
            throw e;
        }
        jsTestLog("setFCV state finished");
    };

    $config.states.untrackUnshardedCollection = function (db, collName, connCache) {
        let tid = this.tid;
        while (tid === this.tid) tid = Random.randInt(this.threadCount);

        const targetThreadColl = this.threadCollectionName(collName, tid);
        const namespace = `${db}.${targetThreadColl}`;
        jsTest.log.info(`Started to untrack collection ${namespace}`);
        // Attempt to unshard the collection first
        jsTest.log.info(`1. Attempting to unshard collection ${namespace}`);
        const res = assert.commandWorkedOrFailedWithCode(
            db.adminCommand({unshardCollection: namespace}),
            this.kReshardingAcceptableErrors,
        );
        if (!res.ok && kReshardingSetFcvErrors.includes(res.code)) {
            jsTest.log.info(
                `Unsharding failed due to concurrent setFCV, performing an early exit since the operation cannot safely continue`,
            );
            return;
        }
        jsTest.log.info(`Unsharding completed ${namespace}`);
        jsTest.log.info(`2. Untracking collection ${namespace}`);
        // Note this command will behave as no-op in case the collection is not tracked.
        assert.commandWorkedOrFailedWithCode(
            db.adminCommand({untrackUnshardedCollection: namespace}),
            [
                // Handles the case where the collection is not located on its primary
                ErrorCodes.OperationFailed,
                // Handles the case where the collection is sharded
                ErrorCodes.InvalidNamespace,
                // Handles the case where the collection/db does not exist
                ErrorCodes.NamespaceNotFound,
            ],
        );
        jsTest.log.info(`Untrack collection completed`);
    };

    $config.transitions = uniformDistTransitions($config.states);

    $config.teardown = function (db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
    };

    return $config;
});
