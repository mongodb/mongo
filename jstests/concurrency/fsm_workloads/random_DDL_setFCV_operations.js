'use strict';

/**
 * Concurrently performs DDL commands and FCV changes and verifies guarantees are
 * not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   # TODO (SERVER-56879) Support add/remove shards in new DDL paths
 *   does_not_support_add_remove_shards,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-75391) Re-enable this test.
 *   config_shard_incompatible,
 *  ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_DDL_operations.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.states.setFCV = function(db, collName, connCache) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog('Executing FCV state, setting to:' + targetFCV);
        try {
            assertAlways.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: targetFCV}));
        } catch (e) {
            if (e.code === 5147403) {
                // Invalid fcv transition (e.g lastContinuous -> lastLTS)
                jsTestLog('setFCV: Invalid transition');
                return;
            }
            if (e.code === 7428200) {
                // Cannot upgrade FCV if a previous FCV downgrade stopped in the middle of cleaning
                // up internal server metadata.
                assertAlways.eq(latestFCV, targetFCV);
                jsTestLog(
                    'setFCV: Cannot upgrade FCV if a previous FCV downgrade stopped in the middle \
                    of cleaning up internal server metadata');
                return;
            }
            throw e;
        }

        jsTestLog('setFCV state finished');
    };

    // TODO (SERVER-73875): Include `movePrimary` state once 7.0 becomes last LTS.
    $config.transitions = {
        create: {create: 0.225, drop: 0.225, rename: 0.225, collMod: 0.225, setFCV: 0.10},
        drop: {create: 0.225, drop: 0.225, rename: 0.225, collMod: 0.225, setFCV: 0.10},
        rename: {create: 0.225, drop: 0.225, rename: 0.225, collMod: 0.225, setFCV: 0.10},
        collMod: {create: 0.225, drop: 0.225, rename: 0.225, collMod: 0.225, setFCV: 0.10},
        setFCV: {create: 0.225, drop: 0.225, rename: 0.225, collMod: 0.225, setFCV: 0.10}
    };

    $config.teardown = function(db, collName, cluster) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    };

    return $config;
});
