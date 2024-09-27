'use strict';

/**
 * This test performs random updates and deletes (see random_update_delete.js) on a sharded
 * collection while performing random chunk migrations. The pauseMigrationsDuringMultiUpdates
 * cluster parameter will be enabled for the duration of the test, so this is not compatible with
 * concurrency_simultaneous, since this would potentially interfere with other tests.
 *
 * @tags: [
 * requires_sharding,
 * assumes_balancer_off,
 * incompatible_with_concurrency_simultaneous,
 * requires_fcv_80
 * ];
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_base.js";
import {migrationsAreAllowed} from "jstests/libs/chunk_manipulation_util.js";
import {
    randomUpdateDelete
} from "jstests/concurrency/fsm_workload_modifiers/random_update_delete.js";

function getPauseMigrationsClusterParameter(db) {
    const response = assert.commandWorked(
        db.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));
    return response.clusterParameters[0].enabled;
}

function setPauseMigrationsClusterParameter(db, cluster, enabled) {
    assert.commandWorked(
        db.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled}}}));

    cluster.executeOnMongosNodes((db) => {
        // Ensure all mongoses have refreshed cluster parameter after being set.
        assert.soon(() => {
            return getPauseMigrationsClusterParameter(db) === enabled;
        });
    });
}

const $partialConfig = extendWorkload($baseConfig, randomUpdateDelete);

export const $config = extendWorkload($partialConfig, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;
    $config.data.partitionSize = 100;

    $config.data.isMoveChunkErrorAcceptable = (err) => {
        return err.code === ErrorCodes.Interrupted;
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        setPauseMigrationsClusterParameter(db, cluster, true);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);
        assert(migrationsAreAllowed(db, collName));
        setPauseMigrationsClusterParameter(db, cluster, false);
    };

    const weights = {moveChunk: 0.2, performUpdates: 0.4, performDeletes: 0.4};
    $config.transitions = {
        init: weights,
        moveChunk: weights,
        performUpdates: weights,
        performDeletes: weights,
    };

    return $config;
});
