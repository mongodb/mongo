/**
 * Test that the shutdown command called on a primary node waits for a majority of secondaries to
 * catch up before taking effect, and will fail otherwise.
 *
 * 1.  Initiate a 3-node replica set
 * 2.  Block replication to secondaries
 * 3.  Write to primary
 * 4.  Try to shut down primary and expect failure
 * 5.  Try to shut down primary in a parallel shell and expect success
 * 6.  Resume replication on secondaries
 * 7.  Try to create a new connection to the shut down primary and expect an error
 *
 */
(function() {
    load("jstests/libs/write_concern_util.js");  // for stopReplicationOnSecondaries,
                                                 // restartReplicationOnSecondaries

    "use strict";

    var name = "shutdown_primary";

    var replTest = new ReplSetTest({name: name, nodes: 3});
    replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var testDB = primary.getDB(name);
    var timeout = 5 * 60 * 1000;
    assert.writeOK(testDB.foo.insert({x: 1}, {writeConcern: {w: 3, wtimeout: timeout}}));

    jsTestLog("Blocking replication to secondaries.");
    stopReplicationOnSecondaries(replTest);

    jsTestLog("Executing write to primary.");
    assert.writeOK(testDB.foo.insert({x: 2}));

    jsTestLog("Attempting to shut down primary.");
    assert.commandFailedWithCode(primary.adminCommand({shutdown: 1}),
                                 ErrorCodes.ExceededTimeLimit,
                                 "shut down did not fail with 'ExceededTimeLimit'");

    jsTestLog("Verifying primary did not shut down.");
    assert.writeOK(testDB.foo.insert({x: 3}));

    jsTestLog("Shutting down primary in a parallel shell");
    var awaitShell = startParallelShell(function() {
        db.adminCommand({shutdown: 1, timeoutSecs: 60});
    }, primary.port);

    jsTestLog("Resuming replication.");
    restartReplicationOnSecondaries(replTest);

    jsTestLog("Verifying primary shut down and cannot be connected to.");
    // Successfully starting shutdown throws a network error.
    var exitCode = awaitShell({checkExitSuccess: false});
    assert.neq(0, exitCode, "expected shutdown to close the shell's connection");
    assert.soonNoExcept(function() {
        // The parallel shell exits while shutdown is in progress, and if this happens early enough,
        // the primary can still accept connections despite successfully starting to shutdown.
        // So, retry connecting until connections cannot be established and an error is thrown.
        assert.throws(function() {
            new Mongo(primary.host);
        });
        return true;
    }, "expected primary node to shut down and not be connectable");

    replTest.stopSet();
})();
