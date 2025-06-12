/**
 * Tests that collection metadata changes don't crash multiplan rate limiter. Collection metadata
 * changes are potentially risky because MultiPlanBucket is a decoration on a Collection instance.
 *
 * @tags: [
 *  requires_fcv_82,
 *  featureFlagMultiPlanLimiter,
 *  requires_getmore,
 *  incompatible_with_concurrency_simultaneous,
 *  assumes_stable_shard_list,
 *  does_not_support_stepdowns,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/multi_plan_storm.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.states = {
        query: function query(db, collName) {
            try {
                $super.states.query.apply(this, [db, collName]);
            } catch (e) {
                const allowedCodes = [
                    // Expected behavior when collection metadata changes while planning is in
                    // progress.
                    ErrorCodes.QueryPlanKilled,
                    // May happen in multi statement transaction suites.
                    ErrorCodes.LockTimeout,
                    ErrorCodes.ExceededTimeLimit,
                    ErrorCodes.StaleConfig,
                ];
                assert.contains(e.code, allowedCodes);
            }
        },
        recreateIndex: function recreateIndex(db, collName) {
            // Both commands may fail due to a concurrent dropIndex.
            assert.commandWorkedOrFailedWithCode(db[collName].dropIndex({a: 1}),
                                                 [ErrorCodes.IndexNotFound]);
            assert.commandWorkedOrFailedWithCode(db[collName].createIndex({a: 1}),
                                                 [ErrorCodes.IndexBuildAborted]);
        },
        collMod: function collMod(db, collName) {
            // Change the validation level.
            const validationLevels = ['off', 'strict', 'moderate'];
            const newValidationLevel = validationLevels[Random.randInt(validationLevels.length)];
            jsTestLog(`Running collMod: coll=${collName} validationLevel=${newValidationLevel}`);
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({collMod: collName, validationLevel: newValidationLevel}),
                [ErrorCodes.ConflictingOperationInProgress]);
        }
    };

    $config.transitions = {
        query: {query: 0.9, recreateIndex: 0.05, collMod: 0.05},
        recreateIndex: {query: 1},
        collMod: {query: 1},
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, [db, collName, cluster]);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, [db, collName, cluster]);
    };

    $config.iterations = 5;

    return $config;
});
