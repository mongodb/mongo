/**
 * Concurrently performs CRUD operations, DDL commands and FCV changes and verifies guarantees are
 * not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_balancer_off,
 *   does_not_support_causal_consistency,
 *   # The mutex mechanism used in CRUD and drop states does not support stepdown
 *   does_not_support_stepdowns,
 *   # Can be removed once PM-1965-Milestone-1 is completed.
 *   does_not_support_transactions,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-88964, SERVER-90971, SERVER-91702, SERVER-87931): Enable this test
 *   exclude_when_record_ids_replicated,
 *   # Relies on internalInsertMaxBatchSize to be 64 or above, but it may be fuzzed to lower values.
 *   does_not_support_config_fuzzer,
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {assertSetFCVSoon} from "jstests/concurrency/fsm_workload_helpers/query/assert_fcv_reset_soon.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/random_ddl_crud_operations.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.setFCV = function (db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog("setFCV to " + targetFCV);
        try {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
        } catch (e) {
            if (handleRandomSetFCVErrors(e, targetFCV)) return;
            throw e;
        }
        jsTestLog("setFCV state finished");
    };

    $config.transitions = {
        init: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        create: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        CRUD: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        drop: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        rename: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        setFCV: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
    };

    $config.teardown = function (db, collName, cluster) {
        assertSetFCVSoon(db, latestFCV);
    };

    return $config;
});
