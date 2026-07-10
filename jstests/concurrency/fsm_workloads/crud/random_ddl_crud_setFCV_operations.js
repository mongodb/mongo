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

    $config.transitions = uniformDistTransitions($config.states);

    $config.teardown = function (db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
    };

    return $config;
});
