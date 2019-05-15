/**
 * Tests invalid getMore attempts against an established global snapshot cursor on mongos. The
 * cursor should still be valid and usable after each failed attempt.
 */
function verifyInvalidGetMoreAttempts(mainDb, collName, cursorId, lsid, txnNumber) {
    // Reject getMores without a session.
    assert.commandFailedWithCode(
        mainDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1}), 50800);

    // Subsequent getMore requests without the same session id are rejected. The cursor should
    // still be valid and usable after this failed attempt.
    assert.commandFailedWithCode(
        mainDb.runCommand(
            {getMore: cursorId, collection: collName, batchSize: 1, lsid: {id: UUID()}}),
        50801);

    // Reject getMores without txnNumber.
    assert.commandFailedWithCode(
        mainDb.runCommand({getMore: cursorId, collection: collName, batchSize: 1, lsid: lsid}),
        50803);

    // Reject getMores without same txnNumber. This fails with NoSuchTransaction because the
    // txnNumber 50 is higher than the active txnNumber for the session.
    assert.commandFailedWithCode(mainDb.runCommand({
        getMore: cursorId,
        collection: collName,
        batchSize: 1,
        lsid: lsid,
        txnNumber: NumberLong(50),
        autocommit: false
    }),
                                 ErrorCodes.NoSuchTransaction);
}
