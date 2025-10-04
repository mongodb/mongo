/**
 * Utilities for testing retryable writes.
 */
export var RetryableWritesUtil = (function () {
    /**
     * Returns true if the error code is retryable, assuming the command is idempotent.
     *
     * TODO SERVER-34666: Expose the isRetryableCode() function that's defined in
     * src/mongo/shell/session.js and use it here.
     */
    function isRetryableCode(code) {
        return (
            ErrorCodes.isNetworkError(code) ||
            ErrorCodes.isNotPrimaryError(code) ||
            ErrorCodes.isWriteConcernError(code) ||
            ErrorCodes.isShutdownError(code) ||
            ErrorCodes.isInterruption(code)
        );
    }

    // The names of all codes that return true in isRetryableCode() above. Can be used where the
    // original error code is buried in a response's error message.
    const kRetryableCodeNames = Object.keys(ErrorCodes).filter((codeName) => {
        return isRetryableCode(ErrorCodes[codeName]);
    });

    // Returns true if the error message contains a retryable code name.
    function errmsgContainsRetryableCodeName(errmsg) {
        return (
            typeof errmsg !== "undefined" &&
            kRetryableCodeNames.some((codeName) => {
                return errmsg.indexOf(codeName) > 0;
            })
        );
    }

    const kRetryableWriteCommands = new Set([
        "delete",
        "findandmodify",
        "findAndModify",
        "insert",
        "update",
        "testInternalTransactions",
        "bulkWrite",
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

        assert.eq(
            bsonWoCompare(primaryRecord, secondaryRecord),
            0,
            "expected transaction records: " +
                tojson(primaryRecord) +
                " and " +
                tojson(secondaryRecord) +
                " to be the same for lsid: " +
                tojson(lsid),
        );
    }

    /**
     * Runs the provided retriable command nTimes. This assumes that the the provided conn
     * was started with `retryWrites: false` to mimic the retry functionality manually.
     */
    function runRetryableWrite(conn, command, expectedErrorCode = ErrorCodes.OK, nTimes = 2) {
        let res;
        for (let i = 0; i < nTimes; i++) {
            jsTestLog(
                "Executing command: " + tojson(command) + "\nIteration: " + i + "\nExpected Code: " + expectedErrorCode,
            );
            res = conn.runCommand(command);
        }
        if (expectedErrorCode === ErrorCodes.OK) {
            assert.commandWorked(res);
        } else {
            assert.commandFailedWithCode(res, expectedErrorCode);
        }
        return res;
    }

    function isFailedToSatisfyPrimaryReadPreferenceError(res) {
        const kReplicaSetMonitorError = /Could not find host matching read preference.*mode:.*primary/;
        if (res.code === ErrorCodes.FailedToSatisfyReadPreference && res.hasOwnProperty("reason")) {
            return res.reason.match(kReplicaSetMonitorError);
        }
        if (res.hasOwnProperty("errmsg")) {
            return res.errmsg.match(kReplicaSetMonitorError);
        }
        if (res.hasOwnProperty("message")) {
            return res.message.match(kReplicaSetMonitorError);
        }
        if (res.hasOwnProperty("writeErrors")) {
            for (let writeError of res.writeErrors) {
                if (writeError.errmsg.match(kReplicaSetMonitorError)) {
                    return true;
                }
            }
        }
        return false;
    }

    function retryOnRetryableCode(fn, prefix) {
        let ret;
        assert.soon(() => {
            try {
                ret = fn();
                return true;
            } catch (e) {
                if (RetryableWritesUtil.isRetryableCode(e.code)) {
                    jsTest.log.info(prefix, {error: e});
                    return false;
                }
                throw e;
            }
        });
        return ret;
    }

    // Expected to be called with a response for an "explain" command.
    function shouldRetryExplainCommand(res) {
        // Several commands that use the plan executor swallow the actual error code from a failed
        // plan into their error message and instead return OperationFailed.
        //
        // TODO SERVER-32208: Remove this function once it is no longer needed.
        const isRetryableExecutorCodeAndMessage = (code, msg) => {
            return (
                code === ErrorCodes.OperationFailed &&
                typeof msg !== "undefined" &&
                msg.indexOf("InterruptedDueToReplStateChange") >= 0
            );
        };

        // If an explain is interrupted by a stepdown, and it returns before its connection is
        // closed, it will return incomplete results. To prevent failing the test, force retries
        // of interrupted explains.
        if (res.hasOwnProperty("executionStats")) {
            const shouldRetryExplain = function (executionStats) {
                return (
                    !executionStats.executionSuccess &&
                    (isRetryableCode(executionStats.errorCode) ||
                        isRetryableExecutorCodeAndMessage(executionStats.errorCode, executionStats.errorMessage))
                );
            };
            const executionStats = res.executionStats.executionStages.hasOwnProperty("shards")
                ? res.executionStats.executionStages.shards
                : [res.executionStats];

            if (executionStats.some(shouldRetryExplain)) {
                jsTest.log("Forcing retry of interrupted explain");
                return true;
            }
        }

        // An explain command can fail if its child command cannot be run on the current server.
        // This can be hit if a primary only or not explicitly slaveOk command is accepted by a
        // primary node that then steps down and returns before having its connection closed.
        if (!res.ok && res.errmsg.indexOf("child command cannot run on this node") >= 0) {
            jsTest.log("Forcing retry of explain likely interrupted by transition to secondary");
            return true;
        }
        return false;
    }

    function runCommandWithRetries(conn, cmd) {
        return retryOnRetryableCode(
            () => assert.commandWorked(conn.runCommand(cmd)),
            "Retry interrupt: runCommand(" + tojson(cmd) + ")",
        );
    }

    return {
        isRetryableCode,
        errmsgContainsRetryableCodeName,
        isRetryableWriteCmdName,
        checkTransactionTable,
        assertSameRecordOnBothConnections,
        runRetryableWrite,
        isFailedToSatisfyPrimaryReadPreferenceError,
        retryOnRetryableCode,
        shouldRetryExplainCommand,
        runCommandWithRetries,
    };
})();
