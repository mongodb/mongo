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
        return ErrorCodes.isNetworkError(code) || ErrorCodes.isNotPrimaryError(code) ||
            ErrorCodes.isWriteConcernError(code) || ErrorCodes.isShutdownError(code) ||
            ErrorCodes.isInterruption(code);
    }

    // The names of all codes that return true in isRetryableCode() above. Can be used where the
    // original error code is buried in a response's error message.
    const kRetryableCodeNames = Object.keys(ErrorCodes).filter((codeName) => {
        return isRetryableCode(ErrorCodes[codeName]);
    });

    // Returns true if the error message contains a retryable code name.
    function errmsgContainsRetryableCodeName(errmsg) {
        return typeof errmsg !== "undefined" && kRetryableCodeNames.some(codeName => {
            return errmsg.indexOf(codeName) > 0;
        });
    }

    const kRetryableWriteCommands = new Set([
        "delete",
        "findandmodify",
        "findAndModify",
        "insert",
        "update",
        "testInternalTransactions",
        "bulkWrite"
    ]);

    /**
     * Returns true if the command name is that of a retryable write command.
     */
    function isRetryableWriteCmdName(cmdName) {
        return kRetryableWriteCommands.has(cmdName);
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

    /**
     * Runs the provided retriable command nTimes. This assumes that the the provided conn
     * was started with `retryWrites: false` to mimic the retry functionality manually.
     */
    function runRetryableWrite(conn, command, expectedErrorCode = ErrorCodes.OK, nTimes = 2) {
        var res;
        for (var i = 0; i < nTimes; i++) {
            jsTestLog("Executing command: " + tojson(command) + "\nIteration: " + i +
                      "\nExpected Code: " + expectedErrorCode);
            res = conn.runCommand(command);
        }
        if (expectedErrorCode === ErrorCodes.OK) {
            assert.commandWorked(res);
        } else {
            assert.commandFailedWithCode(res, expectedErrorCode);
        }
    }

    return {
        isRetryableCode,
        errmsgContainsRetryableCodeName,
        isRetryableWriteCmdName,
        checkTransactionTable,
        assertSameRecordOnBothConnections,
        runRetryableWrite,
    };
})();
