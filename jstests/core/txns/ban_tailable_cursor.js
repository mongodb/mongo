// Check that opening a tailable cursor within a transaction is not allowed.
// @tags: [uses_transactions]
(function() {
    const dbName = 'test';
    const collName = 'tailable-cursor-ban';

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    sessionColl.drop();
    assert.commandWorked(sessionDb.runCommand(
        {create: collName, writeConcern: {w: "majority"}, capped: true, size: 2048}));

    // Try opening a tailable cursor in a transaction.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({x: 1}));

    const cmdRes =
        sessionDb.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
    assert.commandFailedWithCode(cmdRes, ErrorCodes.InvalidOptions);

    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Try opening a change stream in a transaction.
    session.startTransaction();
    assert.commandFailedWithCode(sessionColl.runCommand({
        aggregate: sessionColl.getName(),
        pipeline: [{$changeStream: {}}],
        cursor: {batchSize: 0},
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.endSession();
})();
