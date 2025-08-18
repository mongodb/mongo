/**
 * Repeatedly creates and drops a database in concurrency with FCV changes
 *
 * @tags: [
 *   requires_sharding,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-104171) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # TODO (SERVER-88964, SERVER-90971, SERVER-91702, SERVER-87931): Enable this test.
 *   exclude_when_record_ids_replicated,
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    handleRandomSetFCVErrors
} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";
import {
    assertSetFCVSoon
} from "jstests/concurrency/fsm_workload_helpers/query/assert_fcv_reset_soon.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/ddl/drop_database/drop_database_sharded.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states.setFCV = function(db, collName) {
        const fcvValues = [lastLTSFCV, lastContinuousFCV, latestFCV];
        const targetFCV = fcvValues[Random.randInt(3)];
        jsTestLog('Executing FCV state, setting to:' + targetFCV);
        try {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
        } catch (e) {
            if (handleRandomSetFCVErrors(e, targetFCV))
                return;
            throw e;
        }
        jsTestLog('setFCV state finished');
    };

    // Inherithed methods get overridden to tolerate the interruption of
    // internal transactions on the config server during the execution of setFCV.
    $config.states.enableSharding = function(db, collName) {
        try {
            $super.states.enableSharding.apply(this, arguments);
        } catch (err) {
            if (err.code !== ErrorCodes.Interrupted) {
                throw err;
            }
        }
    };

    $config.states.shardCollection = function(db, collName) {
        try {
            $super.states.shardCollection.apply(this, arguments);
        } catch (err) {
            if (err.code !== ErrorCodes.Interrupted) {
                throw err;
            }
        }
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
        assertSetFCVSoon(db, latestFCV);
        $super.teardown(db, collName);
    };

    return $config;
});
