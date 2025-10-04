/**
 * Concurrently performs DDL commands and FCV changes and verifies guarantees are
 * not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-104171) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # TODO (SERVER-88964, SERVER-90971, SERVER-91702, SERVER-87931): Enable this test
 *   exclude_when_record_ids_replicated
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {assertSetFCVSoon} from "jstests/concurrency/fsm_workload_helpers/query/assert_fcv_reset_soon.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/ddl/random_ddl/random_ddl_operations.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // TODO SERVER-111230 Re-enable state execution.
    delete $config.states.untrackUnshardedCollection;

    $config.states.setFCV = function (db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog("Executing FCV state, setting to:" + targetFCV);
        try {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
        } catch (e) {
            if (handleRandomSetFCVErrors(e, targetFCV)) return;
            throw e;
        }

        jsTestLog("setFCV state finished");
    };

    $config.transitions = uniformDistTransitions($config.states);

    $config.teardown = function (db, collName, cluster) {
        assertSetFCVSoon(db, latestFCV);
    };

    return $config;
});
