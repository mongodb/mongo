/**
 * Tests that journaled write operations that have occurred since the last checkpoint are replayed
 * when the merizod is killed and restarted with --nojournal.
 */
(function() {
    'use strict';

    // Skip this test if not running with the "wiredTiger" storage engine.
    if (jsTest.options().storageEngine && jsTest.options().storageEngine !== 'wiredTiger') {
        jsTest.log('Skipping test because storageEngine is not "wiredTiger"');
        return;
    }

    // Returns a function that primarily executes unjournaled inserts, but periodically does a
    // journaled insert. If 'checkpoint' is true, then the fsync command is run to create a
    // checkpoint prior to the merizod being terminated.
    function insertFunctionFactory(checkpoint) {
        var insertFunction = function() {
            for (var iter = 0; iter < 1000; ++iter) {
                var bulk = db.nojournal.initializeUnorderedBulkOp();
                for (var i = 0; i < 100; ++i) {
                    bulk.insert({unjournaled: i});
                }
                assert.writeOK(bulk.execute({j: false}));
                assert.writeOK(db.nojournal.insert({journaled: iter}, {writeConcern: {j: true}}));
                if (__checkpoint_template_placeholder__ && iter === 50) {
                    assert.commandWorked(db.adminCommand({fsync: 1}));
                }
            }
        };

        return '(' +
            insertFunction.toString().replace('__checkpoint_template_placeholder__',
                                              checkpoint.toString()) +
            ')();';
    }

    function runTest(options) {
        var dbpath = MerizoRunner.dataPath + 'wt_nojournal_toggle';
        resetDbpath(dbpath);

        // Start a merizod with journaling enabled.
        var conn = MerizoRunner.runMerizod({
            dbpath: dbpath,
            noCleanData: true,
            journal: '',
        });
        assert.neq(null, conn, 'merizod was unable to start up');

        // Run a mixture of journaled and unjournaled write operations against the merizod.
        var awaitShell = startParallelShell(insertFunctionFactory(options.checkpoint), conn.port);

        // After some journaled write operations have been performed against the merizod, send a
        // SIGKILL to the process to trigger an unclean shutdown.
        assert.soon(function() {
            var testDB = conn.getDB('test');
            var count = testDB.nojournal.count({journaled: {$exists: true}});
            if (count >= 100) {
                // We saw 100 journaled inserts, but visibility does not guarantee durability, so
                // do an extra journaled write to make all visible commits durable, before killing
                // the merizod.
                assert.writeOK(testDB.nojournal.insert({final: true}, {writeConcern: {j: true}}));
                MerizoRunner.stopMerizod(conn, 9, {allowedExitCode: MerizoRunner.EXIT_SIGKILL});
                return true;
            }
            return false;
        }, 'the parallel shell did not perform at least 100 journaled inserts');

        var exitCode = awaitShell({checkExitSuccess: false});
        assert.neq(0, exitCode, 'expected shell to exit abnormally due to merizod being terminated');

        // Restart the merizod with journaling disabled.
        conn = MerizoRunner.runMerizod({
            dbpath: dbpath,
            noCleanData: true,
            nojournal: '',
        });
        assert.neq(null, conn, 'merizod was unable to restart after receiving a SIGKILL');

        var testDB = conn.getDB('test');
        assert.eq(1, testDB.nojournal.count({final: true}), 'final journaled write was not found');
        assert.lte(100,
                   testDB.nojournal.count({journaled: {$exists: true}}),
                   'journaled write operations since the last checkpoint were not replayed');

        var initialNumLogWrites = testDB.serverStatus().wiredTiger.log['log write operations'];
        assert.writeOK(testDB.nojournal.insert({a: 1}, {writeConcern: {fsync: true}}));
        assert.eq(initialNumLogWrites,
                  testDB.serverStatus().wiredTiger.log['log write operations'],
                  'journaling is still enabled even though --nojournal was specified');

        MerizoRunner.stopMerizod(conn);

        // Restart the merizod with journaling enabled.
        conn = MerizoRunner.runMerizod({
            dbpath: dbpath,
            noCleanData: true,
            journal: '',
        });
        assert.neq(null, conn, 'merizod was unable to start up after re-enabling journaling');

        // Change the database object to connect to the restarted merizod.
        testDB = conn.getDB('test');
        initialNumLogWrites = testDB.serverStatus().wiredTiger.log['log write operations'];

        assert.writeOK(testDB.nojournal.insert({a: 1}, {writeConcern: {fsync: true}}));
        assert.lt(initialNumLogWrites,
                  testDB.serverStatus().wiredTiger.log['log write operations'],
                  'journaling is still disabled even though --journal was specified');

        MerizoRunner.stopMerizod(conn);
    }

    // Operations from the journal should be replayed even when the merizod is terminated before
    // anything is written to disk.
    jsTest.log('Running the test without ever creating a checkpoint');
    runTest({checkpoint: false});

    // Repeat the test again, but ensure that some data is written to disk before the merizod is
    // terminated.
    jsTest.log('Creating a checkpoint part-way through running the test');
    runTest({checkpoint: true});
})();
