/**
 * Test that transactions are prohibited from running on secondaries.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    // In 4.0, we allow read-only transactions on secondaries when test commands are enabled, so we
    // disable them in this test, to test that transactions on secondaries will be disallowed
    // for production users.
    jsTest.setOption('enableTestCommands', false);
    TestData.authenticationDatabase = "local";

    const dbName = "test";
    const collName = "transactions_prohibited_on_secondaries";

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
    assert.commandWorked(primary.getDB(dbName).createCollection(collName));
    replTest.awaitLastOpCommitted();

    // Initiate a session on the secondary.
    const sessionOptions = {causalConsistency: false};
    const session = secondary.getDB(dbName).getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    let txnNumber = 0;

    /**
     * Verify that all given commands are disallowed from starting a transaction on a secondary by
     * checking that each command fails with the expected error code.
     */
    function testCommands(commands, expectedErrorCode) {
        // Verify secondary transactions are disallowed various command types.
        for (let i = 0; i < commands.length; i++) {
            let startTxnArgs = {
                readConcern: {level: "snapshot"},
                txnNumber: NumberLong(txnNumber++),
                stmtId: NumberInt(0),
                startTransaction: true,
                autocommit: false,
            };
            let cmdObject = Object.assign(commands[i], startTxnArgs);
            jsTestLog("Trying to start transaction on secondary with command: " +
                      tojson(cmdObject));
            assert.commandFailedWithCode(sessionDb.runCommand(cmdObject), expectedErrorCode);
        }
    }

    // Test read commands that are supported in transactions.
    let readCommands = [
        {find: collName},
        {count: collName},
        {aggregate: collName, pipeline: [{$project: {_id: 1}}], cursor: {}},
        {distinct: collName, key: "_id"},
    ];

    jsTestLog("Testing read commands.");
    testCommands(readCommands, 50789);

    // Test one write command. Normal write commands should already be
    // disallowed on secondaries so we don't test them exhaustively here.
    let writeCommands = [{insert: collName, documents: [{_id: 0}]}];

    jsTestLog("Testing write commands.");
    testCommands(writeCommands, 50789);

    session.endSession();
    replTest.stopSet();
}());
