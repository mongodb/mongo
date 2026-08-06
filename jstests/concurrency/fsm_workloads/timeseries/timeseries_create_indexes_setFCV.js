/**
 * Repeatedly create indexes while dropping and recreating timeseries collections, with
 * FCV upgrade/downgrade happening in the background.
 * This is designed to exercise viewless timeseries upgrade/downgrade.
 * TODO(SERVER-114573): Consider removing this test once 9.0 becomes lastLTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   # setFCV requires all nodes on the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-104171) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # Runs setFCV, which can interfere with other tests.
 *   incompatible_with_concurrency_simultaneous,
 *   runs_set_fcv,
 *   # TODO SERVER-105509 enable test in config shard suites
 *   config_shard_incompatible,
 *   creates_background_indexes,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/timeseries/timeseries_create_indexes.js";
import {setFCVWithRetryOnBackgroundOpInProgress} from "jstests/libs/set_fcv_helpers.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.setFCV = function (db, collName) {
        const fcvValues = [lastLTSFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(2)];
        jsTest.log.info("Executing FCV state, setting to:" + targetFCV);
        try {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
            );
        } catch (e) {
            if (handleRandomSetFCVErrors(e, targetFCV)) return;
            throw e;
        }
        jsTest.log.info("setFCV state finished");
    };

    $config.transitions = uniformDistTransitions($config.states);

    $config.teardown = function teardown(db, collName, cluster) {
        // TODO(SERVER-114573): Remove once v9.0 is last LTS and viewless timeseries upgrade/downgrade doesn't happen.
        setFCVWithRetryOnBackgroundOpInProgress(db, latestFCV);
        $super.teardown.apply(this, arguments);
    };

    return $config;
});
