/**
 * Executes a series of placement-changing DDLs while resetPlacementHistory runs in parallel.
 * After tearing down the test, the check_routing_table_consistency hook will verify that
 * the content config.placementHistory will still be consistent with the rest of the catalog.
 *
 * @tags: [
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/ddl/random_ddl/random_ddl_operations.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Use multiple databases to increase the chance of collisions when running resetPlacementHistory.
    $config.data.dbCount = 3;

    // Metadata consistency checks are not relevant to qualify the behavior of resetPlacementHistory and may be avoided.
    delete $config.states.checkDatabaseMetadataConsistency;
    delete $config.states.checkCollectionMetadataConsistency;

    $config.states.resetPlacementHistory = function (db, collName, connCache) {
        jsTest.log.info(`Executing resetPlacementHistory`);
        assert.commandWorked(db.adminCommand({resetPlacementHistory: 1}));
        jsTest.log.info(`resetPlacementHistory completed`);
    };

    $config.transitions = uniformDistTransitions($config.states);
    return $config;
});
