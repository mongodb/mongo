'use strict';

/**
 * collmod_writeconflict.js
 *
 * Ensures collMod successfully handles WriteConflictExceptions.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/collmod.js');     // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'collmod_writeconflict';
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        // Log traces for each WriteConflictException encountered in case they are not handled
        // properly.
        assertAlways.commandWorked(
            db.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}));

        // Set up failpoint to trigger WriteConflictException during write operations.
        assertAlways.commandWorked(db.adminCommand(
            {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: 0.5}}));
    };
    $config.teardown = function teardown(db, collName, cluster) {
        assertAlways.commandWorked(
            db.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));
        assertAlways.commandWorked(
            db.adminCommand({setParameter: 1, traceWriteConflictExceptions: false}));
    };

    $config.threadCount = 2;
    $config.iterations = 5;

    return $config;
});
