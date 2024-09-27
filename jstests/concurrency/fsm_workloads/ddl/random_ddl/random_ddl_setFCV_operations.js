/**
 * Concurrently performs DDL commands and FCV changes and verifies guarantees are
 * not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-88539) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # TODO (SERVER-88964, SERVER-90971, SERVER-91702, SERVER-87931): Enable this test
 *   exclude_when_record_ids_replicated
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/ddl/random_ddl/random_ddl_operations.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.setFCV = function(db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog('Executing FCV state, setting to:' + targetFCV);
        try {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
        } catch (e) {
            if (e.code === 5147403) {
                // Invalid fcv transition (e.g lastContinuous -> lastLTS)
                jsTestLog('setFCV: Invalid transition');
                return;
            }
            if (e.code === 7428200) {
                // Cannot upgrade FCV if a previous FCV downgrade stopped in the middle of cleaning
                // up internal server metadata.
                assert.eq(latestFCV, targetFCV);
                jsTestLog(
                    'setFCV: Cannot upgrade FCV if a previous FCV downgrade stopped in the middle \
                    of cleaning up internal server metadata');
                return;
            }
            if (e.code === 12587) {
                // Cannot downgrade FCV that requires a collMod command when index builds are
                // concurrently taking place.
                jsTestLog(
                    'setFCV: Cannot downgrade the FCV that requires a collMod command when index \
                    builds are concurrently running');
                return;
            }
            throw e;
        }

        jsTestLog('setFCV state finished');
    };

    $config.transitions = uniformDistTransitions($config.states);

    $config.teardown = function(db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };

    return $config;
});
