// Storage Node Watchdog common test code
//
load("jstests/watchdog/lib/charybdefs_lib.js");

function testMongoDHang(control, mongod_options) {
    'use strict';

    // Now start MongoD with it enabled at startup
    //
    if (mongod_options.hasOwnProperty("dbPath")) {
        resetDbpath(mongod_options.dbPath);
    }

    var options = {
        setParameter: "watchdogPeriodSeconds=" + control.getWatchdogPeriodSeconds(),
        verbose: 1,
    };

    options = Object.extend(mongod_options, options);

    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, 'mongod was unable to start up');

    // Wait for watchdog to get running
    const admin = conn.getDB("admin");

    // Wait for the watchdog to run some checks first
    control.waitForWatchdogToStart(admin);

    // Hang the file system
    control.addWriteDelayFaultAndWait("watchdog_probe.*");

    // Check MongoD is dead by sending SIGTERM
    // This will trigger our "nice" shutdown, but since mongod is stuck in the kernel doing I/O,
    // the process will not terminate until charybdefs is done sleeping.
    print("Stopping MongoDB now, it will terminate once charybdefs is done sleeping.");
    MongoRunner.stopMongod(conn, undefined, {allowedExitCode: EXIT_WATCHDOG});
}

function testFuseAndMongoD(control, mongod_options) {
    'use strict';

    // Cleanup previous runs
    control.cleanup();

    try {
        // Start the file system
        control.start();

        testMongoDHang(control, mongod_options);
    } finally {
        control.cleanup();
    }
}
