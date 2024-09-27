/**
 * collmod_writeconflict.js
 *
 * Ensures collMod successfully handles WriteConflictExceptions.
 *
 * @tags: [
 *  # SERVER-43053 These workloads set a failpoint that causes intermittent WriteConflict errors,
 *  # which presently can cause other simultaneous workloads to fail.
 *  incompatible_with_concurrency_simultaneous,
 *  # The WTWriteConflictException failpoint is not supported on mongos.
 *  assumes_against_mongod_not_mongos,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/collmod.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.prefix = 'collmod_writeconflict';
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        // Log traces for each WriteConflictException encountered in case they are not handled
        // properly.
        /*
          So long as there are no BFs, leave WCE tracing disabled.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}));
        */

        // Set up failpoint to trigger WriteConflictException during write operations.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: 0.5}}));
    };
    $config.teardown = function teardown(db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));
        assert.commandWorked(
            db.adminCommand({setParameter: 1, traceWriteConflictExceptions: false}));
    };

    $config.threadCount = 2;
    $config.iterations = 5;

    return $config;
});
