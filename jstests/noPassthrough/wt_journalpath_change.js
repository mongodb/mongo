/**
 * Tests that journaled write operations that have occurred since the last checkpoint are replayed
 * when the mongod is killed and restarted with --nojournal.
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
    // checkpoint prior to the mongod being terminated.
    function insertFunctionFactory(checkpoint) {
        let insertFunction = function() {
            for (let iter = 0; iter < 1000; ++iter) {
                let bulk = db.nojournal.initializeUnorderedBulkOp();
                for (let i = 0; i < 100; ++i) {
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
        let dbpath = MongoRunner.dataPath + 'wt_nojournal_toggle';
        let journalPath = MongoRunner.dataPath + 'journal';
        resetDbpath(dbpath);

        // Start a mongod with journaling enabled.
        let conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
            journalPath: journalPath,
        });
        assert.neq(null, conn, 'mongod was unable to start up');

        // Run a mixture of journaled and unjournaled write operations against the mongod.
        let awaitShell = startParallelShell(insertFunctionFactory(options.checkpoint), conn.port);

        // After some journaled write operations have been performed against the mongod, send a
        // SIGKILL to the process to trigger an unclean shutdown.
        assert.soon(function() {
            let testDB = conn.getDB('test');
            let count = testDB.nojournal.count({journaled: {$exists: true}});
            if (count >= 100) {
                // We saw 100 journaled inserts, but visibility does not guarantee durability, so
                // do an extra journaled write to make all visible commits durable, before killing
                // the mongod.
                assert.writeOK(testDB.nojournal.insert({final: true}, {writeConcern: {j: true}}));
                MongoRunner.stopMongod(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
                return true;
            }
            return false;
        }, 'the parallel shell did not perform at least 100 journaled inserts');

        let exitCode = awaitShell({checkExitSuccess: false});
        assert.neq(0, exitCode, 'expected shell to exit abnormally due to mongod being terminated');

        // Restarting mongod with a different journal path but the same db path should not be
        // allowed. The proper way to change the journal path is to start mongod with 'nojournal',
        // and then restart it with a different journal path.
        let newJournalPath = MongoRunner.dataPath + 'test_path_change/journal';
        conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
            journalPath: newJournalPath,
        });
        assert.eq(null, conn, 'mongod was able to start despite changing the journal path.');

        // Restart the mongod with journaling disabled. This should force WT to clean up the journal
        // files.
        conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
            nojournal: '',
        });
        assert.neq(null, conn, 'mongod was unable to restart after receiving a SIGKILL');

        let testDB = conn.getDB('test');
        assert.eq(1, testDB.nojournal.count({final: true}), 'final journaled write was not found');
        assert.lte(100,
                   testDB.nojournal.count({journaled: {$exists: true}}),
                   'journaled write operations since the last checkpoint were not replayed');

        let initialNumLogWrites = testDB.serverStatus().wiredTiger.log['log write operations'];
        assert.writeOK(testDB.nojournal.insert({a: 1}, {writeConcern: {fsync: true}}));
        assert.eq(initialNumLogWrites,
                  testDB.serverStatus().wiredTiger.log['log write operations'],
                  'journaling is still enabled even though --nojournal was specified');

        MongoRunner.stopMongod(conn);

        // Restart the mongod with journaling enabled and with a different journalpath. Because
        // mongod was started without a journal path last time, this should work.
        conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
            journalPath: newJournalPath,
        });
        assert.neq(null, conn, 'mongod was unable to start up after re-enabling journaling');

        // Change the database object to connect to the restarted mongod.
        testDB = conn.getDB('test');
        initialNumLogWrites = testDB.serverStatus().wiredTiger.log['log write operations'];

        assert.writeOK(testDB.nojournal.insert({a: 1}, {writeConcern: {fsync: true}}));
        assert.lt(initialNumLogWrites,
                  testDB.serverStatus().wiredTiger.log['log write operations'],
                  'journaling is still disabled even though --journal was specified');

        MongoRunner.stopMongod(conn);
    }

    // Operations from the journal should be replayed even when the mongod is terminated before
    // anything is written to disk.
    jsTest.log('Running the test without ever creating a checkpoint');
    runTest({checkpoint: false});

    // Repeat the test again, but ensure that some data is written to disk before the mongod is
    // terminated.
    jsTest.log('Creating a checkpoint part-way through running the test');
    runTest({checkpoint: true});
})();
