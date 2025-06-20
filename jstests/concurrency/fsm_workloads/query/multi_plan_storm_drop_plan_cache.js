/**
 * Tests that clearing the plan cache doesn't crash multiplan rate limiter.
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
                    // May happen in multi statement transaction suites.
                    ErrorCodes.LockTimeout,
                    ErrorCodes.ExceededTimeLimit,
                    ErrorCodes.StaleConfig,
                    // Expected in balancer suites.
                    ErrorCodes.MigrationConflict,
                ];
                assert.contains(e.code, allowedCodes);
            }
        },
        dropPlanCache: function dropPlanCache(db, collName) {
            db[collName].getPlanCache().clear();
        }
    };

    $config.transitions = {
        query: {query: 0.9, dropPlanCache: 0.1},
        dropPlanCache: {query: 1},
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
