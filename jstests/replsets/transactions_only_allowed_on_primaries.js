/**
 * Test that transactions are only allowed on primaries, and prohibited from running on secondaries.
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
    const collName = "transactions_only_allowed_on_primaries";

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
    const primaryDB = primary.getDB(dbName);
    assert.commandWorked(primary.getDB(dbName).createCollection(collName));
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [
            {name: "geo_2d", key: {geo: "2d"}},
            {key: {haystack: "geoHaystack", a: 1}, name: "haystack_geo", bucketSize: 1}
        ]
    }));
    replTest.awaitLastOpCommitted();

    /**
     * Verify that all given commands are disallowed from starting a transaction on a secondary by
     * checking that each command fails with the expected error code.
     */
    function testCommands(session, commands, expectedErrorCode, readPref) {
        const sessionDb = session.getDatabase(dbName);
        for (let i = 0; i < commands.length; i++) {
            session.startTransaction();
            // Use a read preference that would normally allow read commands to run on secondaries.
            if (readPref !== null) {
                session.getOptions().setReadPreference(readPref);
            }
            const cmdObject = commands[i];

            jsTestLog("Trying to start transaction on secondary with command: " +
                      tojson(cmdObject));
            assert.commandFailedWithCode(sessionDb.runCommand(cmdObject), expectedErrorCode);

            // Call abort for good measure, even though the transaction should have already been
            // aborted on the server.
            session.abortTransaction();
        }
    }

    //
    // Make sure transactions are disallowed on secondaries.
    //

    // Initiate a session on the secondary.
    const sessionOptions = {causalConsistency: false};
    const secondarySession = secondary.getDB(dbName).getMongo().startSession(sessionOptions);

    // Test read commands that are supported in transactions.
    let readCommands = [
        {find: collName},
        {aggregate: collName, pipeline: [{$project: {_id: 1}}], cursor: {}},
        {distinct: collName, key: "_id"},
        {geoSearch: collName, near: [0, 0]}
    ];

    jsTestLog("Testing read commands.");
    // Make sure read commands can not start transactions with any supported read preference.
    testCommands(secondarySession, readCommands, ErrorCodes.NotMaster, "secondary");
    testCommands(secondarySession, readCommands, ErrorCodes.NotMaster, "secondaryPreferred");
    testCommands(secondarySession, readCommands, ErrorCodes.NotMaster, "primaryPreferred");
    testCommands(secondarySession, readCommands, ErrorCodes.NotMaster, null);

    // Test one write command. Normal write commands should already be
    // disallowed on secondaries so we don't test them exhaustively here.
    let writeCommands = [{insert: collName, documents: [{_id: 0}]}];

    jsTestLog("Testing write commands.");
    testCommands(secondarySession, writeCommands, ErrorCodes.NotMaster, "secondary");

    secondarySession.endSession();

    //
    // Make sure transactions are allowed on primaries with any valid read preference.
    //

    const primarySession = primary.getDB(dbName).getMongo().startSession(sessionOptions);
    const primarySessionDb = primarySession.getDatabase(dbName);

    primarySession.startTransaction();
    assert.commandWorked(
        primarySessionDb.runCommand({find: collName, $readPreference: {mode: "primary"}}));
    primarySession.commitTransaction();

    primarySession.startTransaction();
    assert.commandWorked(
        primarySessionDb.runCommand({find: collName, $readPreference: {mode: "primaryPreferred"}}));
    primarySession.commitTransaction();

    primarySession.startTransaction();
    assert.commandWorked(primarySessionDb.runCommand(
        {find: collName, $readPreference: {mode: "secondaryPreferred"}}));
    primarySession.commitTransaction();

    primarySession.startTransaction();
    assert.commandWorked(
        primarySessionDb.runCommand({find: collName, $readPreference: {mode: "nearest"}}));
    primarySession.commitTransaction();

    primarySession.endSession();

    replTest.stopSet();
}());
