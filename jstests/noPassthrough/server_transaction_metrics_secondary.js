// Test that transactions run on secondaries do not change the serverStatus transaction metrics.
// @tags: [uses_transactions]
(function() {
    "use strict";

    jsTest.setOption("enableTestCommands", false);
    TestData.authenticationDatabase = "local";

    const dbName = "test";
    const collName = "server_transaction_metrics_secondary";

    // Start up the replica set. We want a stable topology, so make the secondary unelectable.
    const replTest = new ReplSetTest({name: collName, nodes: 2});
    replTest.startSet();
    let config = replTest.getReplSetConfig();
    config.members[1].priority = 0;
    replTest.initiate(config);

    const primary = replTest.getPrimary();
    const secondary = replTest.getSecondary();

    // Set slaveOk=true so that normal read commands would be allowed on the secondary.
    secondary.setSlaveOk(true);

    // Create a test collection that we can run commands against.
    assert.commandWorked(primary.getDB(dbName)[collName].insert({_id: 0}));
    replTest.awaitLastOpCommitted();

    // Initiate a session on the secondary.
    const sessionOptions = {causalConsistency: false};
    const secondarySession = secondary.getDB(dbName).getMongo().startSession(sessionOptions);
    let secDb = secondarySession.getDatabase(dbName);
    let metrics;

    jsTestLog("Trying to start transaction on secondary.");
    secondarySession.startTransaction();

    // Initially there are no transactions in the system.
    metrics = assert.commandWorked(secondary.adminCommand({serverStatus: 1, repl: 0, metrics: 0}))
                  .transactions;
    assert.eq(0, metrics.currentActive);
    assert.eq(0, metrics.currentInactive);
    assert.eq(0, metrics.currentOpen);
    assert.eq(0, metrics.totalAborted);
    assert.eq(0, metrics.totalCommitted);
    assert.eq(0, metrics.totalStarted);

    jsTestLog("Run transaction statement.");
    assert.eq(assert.throws(() => secDb[collName].findOne({_id: 0})).code, ErrorCodes.NotMaster);

    // The metrics are not affected.
    metrics = assert.commandWorked(secondary.adminCommand({serverStatus: 1, repl: 0, metrics: 0}))
                  .transactions;
    assert.eq(0, metrics.currentActive);
    assert.eq(0, metrics.currentInactive);
    assert.eq(0, metrics.currentOpen);
    assert.eq(0, metrics.totalAborted);
    assert.eq(0, metrics.totalCommitted);
    assert.eq(0, metrics.totalStarted);

    jsTestLog("Abort the transaction.");
    assert.commandFailedWithCode(secondarySession.abortTransaction_forTesting(),
                                 ErrorCodes.NotMaster);

    // The metrics are not affected.
    metrics = assert.commandWorked(secondary.adminCommand({serverStatus: 1, repl: 0, metrics: 0}))
                  .transactions;
    assert.eq(0, metrics.currentActive);
    assert.eq(0, metrics.currentInactive);
    assert.eq(0, metrics.currentOpen);
    assert.eq(0, metrics.totalAborted);
    assert.eq(0, metrics.totalCommitted);
    assert.eq(0, metrics.totalStarted);

    jsTestLog("Done trying transaction on secondary.");
    secondarySession.endSession();

    replTest.stopSet();
}());