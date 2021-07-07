'use strict';

/**
 * Repeatedly creates and drops a database in concurrency with FCV changes
 *
 * @tags: [
 *   requires_sharding,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *  ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/drop_database_sharded.js');
load("jstests/libs/override_methods/mongos_manual_intervention_actions.js");

var $config = extendWorkload($config, function($config, $super) {
    $config.states.setFCV = function(db, collName) {
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
            throw e;
        }
        jsTestLog('setFCV state finished');
    };

    $config.states.shardCollection = function(db, collName) {
        assert.soon(() => {
            try {
                $super.states.shardCollection.apply(this, arguments);
                return true;
            } catch (e) {
                if (e.code === ErrorCodes.ConflictingOperationInProgress) {
                    // Legacy dropCollection (as part of dropDatabase) interferes with catalog cache
                    // refreshes done as part of shardCollection. Retry.
                    // TODO SERVER-54879: No longer needed after 5.0 has branched out
                    return false;
                }
                throw e;
            }
        });
    };

    $config.transitions = {
        init: {enableSharding: 0.3, dropDatabase: 0.3, shardCollection: 0.3, setFCV: 0.1},
        enableSharding: {enableSharding: 0.3, dropDatabase: 0.3, shardCollection: 0.3, setFCV: 0.1},
        dropDatabase: {enableSharding: 0.3, dropDatabase: 0.3, shardCollection: 0.3, setFCV: 0.1},
        shardCollection:
            {enableSharding: 0.3, dropDatabase: 0.3, shardCollection: 0.3, setFCV: 0.1},
        setFCV: {enableSharding: 0.3, dropDatabase: 0.3, shardCollection: 0.3, setFCV: 0.1},
    };

    $config.teardown = function(db, collName) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        $super.teardown(db, collName);
    };

    return $config;
});
