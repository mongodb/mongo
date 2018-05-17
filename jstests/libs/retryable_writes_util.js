/**
 * Utilities for testing retryable writes.
 */
var RetryableWritesUtil = (function() {
    /**
     * Returns true if the error code is retryable, assuming the command is idempotent.
     *
     * TODO SERVER-34666: Expose the isRetryableCode() function that's defined in
     * src/mongo/shell/session.js and use it here.
     */
    function isRetryableCode(code) {
        return ErrorCodes.isNetworkError(code) || ErrorCodes.isNotMasterError(code) ||
            ErrorCodes.isWriteConcernError(code) || ErrorCodes.isShutdownError(code) ||
            ErrorCodes.isInterruption(code);
    }

    const kRetryableWriteCommands =
        new Set(["delete", "findandmodify", "findAndModify", "insert", "update"]);

    /**
     * Returns true if the command name is that of a retryable write command.
     */
    function isRetryableWriteCmdName(cmdName) {
        return kRetryableWriteCommands.has(cmdName);
    }

    const kStorageEnginesWithoutDocumentLocking = new Set(["ephemeralForTest", "mmapv1"]);

    /**
     * Returns true if the given storage engine supports retryable writes (i.e. supports
     * document-level locking).
     */
    function storageEngineSupportsRetryableWrites(storageEngineName) {
        return !kStorageEnginesWithoutDocumentLocking.has(storageEngineName);
    }

    /**
     * Asserts the connection has a document in its transaction collection that has the given
     * sessionId, txnNumber, and lastWriteOptimeTs.
     */
    function checkTransactionTable(conn, lsid, txnNumber, ts) {
        let table = conn.getDB("config").transactions;
        let res = table.findOne({"_id.id": lsid.id});

        assert.eq(res.txnNum, txnNumber);
        assert.eq(res.lastWriteOpTime.ts, ts);
    }

    /**
     * Asserts the transaction collection document for the given session id is the same on both
     * connections.
     */
    function assertSameRecordOnBothConnections(primary, secondary, lsid) {
        let primaryRecord = primary.getDB("config").transactions.findOne({"_id.id": lsid.id});
        let secondaryRecord = secondary.getDB("config").transactions.findOne({"_id.id": lsid.id});

        assert.eq(bsonWoCompare(primaryRecord, secondaryRecord),
                  0,
                  "expected transaction records: " + tojson(primaryRecord) + " and " +
                      tojson(secondaryRecord) + " to be the same for lsid: " + tojson(lsid));
    }

    return {
        isRetryableCode,
        isRetryableWriteCmdName,
        storageEngineSupportsRetryableWrites,
        checkTransactionTable,
        assertSameRecordOnBothConnections,
    };
})();
