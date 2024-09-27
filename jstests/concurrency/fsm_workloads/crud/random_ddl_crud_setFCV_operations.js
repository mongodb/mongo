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
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/crud/random_ddl_crud_operations.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.setFCV = function(db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog('setFCV to ' + targetFCV);
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

    $config.transitions = {
        init: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        create: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        CRUD: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        drop: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        rename: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08},
        setFCV: {create: 0.23, CRUD: 0.23, drop: 0.23, rename: 0.23, setFCV: 0.08}
    };

    $config.teardown = function(db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };

    return $config;
});
