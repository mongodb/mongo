/**
 * Tests that having journaled write operations since the last checkpoint triggers an error when
 * --wiredTigerEngineConfigString log=(recover=error) is specified in combination with --nojournal.
 * Also verifies that deleting the journal/ directory allows those operations to safely be ignored.
 */
(function() {
    'use strict';

    // Skip this test if not running with the "wiredTiger" storage engine.
    if (jsTest.options().storageEngine && jsTest.options().storageEngine !== 'wiredTiger') {
        jsTest.log('Skipping test because storageEngine is not "wiredTiger"');
        return;
    }

    var dbpath = MongoRunner.dataPath + 'wt_nojournal_skip_recovery';
    resetDbpath(dbpath);

    // Start a mongod with journaling enabled.
    var conn = MongoRunner.runMongod({
        dbpath: dbpath,
        noCleanData: true,
        journal: '',
        // Wait an hour between checkpoints to ensure one isn't created after the fsync command is
        // executed and before the mongod is terminated. This is necessary to ensure that exactly 90
        // documents with the 'journaled' field exist in the collection.
        wiredTigerEngineConfigString: 'checkpoint=(wait=3600)'
    });
    assert.neq(null, conn, 'mongod was unable to start up');

    // Execute unjournaled inserts, but periodically do a journaled insert. Triggers a checkpoint
    // prior to the mongod being terminated.
    var awaitShell = startParallelShell(function() {
        for (let loopNum = 1; true; ++loopNum) {
            var bulk = db.nojournal.initializeUnorderedBulkOp();
            for (var i = 0; i < 100; ++i) {
                bulk.insert({unjournaled: i});
            }
            assert.writeOK(bulk.execute({j: false}));
            assert.writeOK(db.nojournal.insert({journaled: loopNum}, {writeConcern: {j: true}}));

            // Create a checkpoint slightly before the mongod is terminated.
            if (loopNum === 90) {
                assert.commandWorked(db.adminCommand({fsync: 1}));
            }
        }
    }, conn.port);

    // After some journaled write operations have been performed against the mongod, send a SIGKILL
    // to the process to trigger an unclean shutdown.
    assert.soon(
        function() {
            var count = conn.getDB('test').nojournal.count({journaled: {$exists: true}});
            if (count >= 100) {
                MongoRunner.stopMongod(conn, 9);
                return true;
            }
            return false;
        },
        'the parallel shell did not perform at least 100 journaled inserts',
        5 * 60 * 1000 /*timeout ms*/);

    var exitCode = awaitShell({checkExitSuccess: false});
    assert.neq(0, exitCode, 'expected shell to exit abnormally due to mongod being terminated');

    // Restart the mongod with journaling disabled, but configure it to error if the database needs
    // recovery.
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        noCleanData: true,
        nojournal: '',
        wiredTigerEngineConfigString: 'log=(recover=error)',
    });
    assert.eq(null, conn, 'mongod should not have started up because it requires recovery');

    // Remove the journal files.
    assert(removeFile(dbpath + '/journal'), 'failed to remove the journal directory');

    // Restart the mongod with journaling disabled again.
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        noCleanData: true,
        nojournal: '',
        wiredTigerEngineConfigString: 'log=(recover=error)',
    });
    assert.neq(null, conn, 'mongod was unable to start up after removing the journal directory');

    var count = conn.getDB('test').nojournal.count({journaled: {$exists: true}});
    assert.lte(90, count, 'missing documents that were present in the last checkpoint');
    assert.gte(90,
               count,
               'journaled write operations since the last checkpoint should not have been' +
                   ' replayed');

    MongoRunner.stopMongod(conn);
})();
