/**
 * Override to run consecutive operations inside the same transaction. When an operation that
 * cannot be run inside of a transaction is encountered, the active transaction is committed
 * before running the next operation.
 */

(function() {
    'use strict';

    load("jstests/libs/error_code_utils.js");
    load("jstests/libs/override_methods/read_and_write_concern_helpers.js");
    load('jstests/libs/override_methods/override_helpers.js');
    load("jstests/libs/retryable_writes_util.js");
    load("jstests/libs/transactions_util.js");

    const runCommandOriginal = Mongo.prototype.runCommand;

    const kCmdsThatInsert = new Set([
        'insert',
        'update',
        'findAndModify',
        'findandmodify',
    ]);

    // Copied from ServerSession.TransactionStates.
    const TransactionStates = {
        kActive: 'active',
        kInactive: 'inactive',
    };

    // Array to hold pairs of (commandObj, makeFuncArgs) that will be iterated
    // over when retrying a command run in a txn on a network error.
    let ops = [];

    // Used to indicate whether the operation is being re-run, so we will not add
    // it to our ops array multple times.
    let retryOp = false;

    // True if the current command returned a transient transaction error and its entire transaction
    // is being retried.
    let retryingOnTransientTransactionError = false;

    // Set the max number of operations to run in a transaction. Once we've
    // hit this number of operations, we will commit the transaction. This is to
    // prevent having to retry an extremely long running transaction.
    const maxOpsInTransaction = 10;

    // The last operation we logged upon failure. To avoid logging a command that
    // fails multiple times in a row each time it fails, we use this check if we've
    // just logged this command. This allows us to log failing commands to help with
    // debugging, but helps to avoid spamming logs.
    let lastLoggedOp;

    // The last TransientTransactionError on a commitTransaction that caused us to retry
    // the entire transaction. For help with debugging.
    let transientErrorToLog;

    // Default read concern level to use for transactions.
    const kDefaultTransactionReadConcernLevel =
        TestData.hasOwnProperty("defaultTransactionReadConcernLevel")
        ? TestData.defaultTransactionReadConcernLevel
        : "snapshot";

    const kDefaultTransactionWriteConcernW =
        TestData.hasOwnProperty("defaultTransactionWriteConcernW")
        ? TestData.defaultTransactionWriteConcernW
        : "majority";

    // Default read concern level to use for commands that are not transactions.
    const kDefaultReadConcernLevel = (function() {
        if (TestData.hasOwnProperty("defaultReadConcernLevel")) {
            return TestData.defaultReadConcernLevel;
        }

        // Use majority if the suite didn't specify a level, unless the variant doesn't support it.
        return TestData.enableMajorityReadConcern !== false ? "majority" : "local";
    })();

    // Default write concern w to use for both transactions and non-transactions.
    const kDefaultWriteConcernW = TestData.hasOwnProperty("defaultWriteConcernW")
        ? TestData.defaultWriteConcernW
        : "majority";

    // Use a "signature" value that won't typically match a value assigned in normal use. This way
    // the wtimeout set by this override is distinguishable in the server logs.
    const kDefaultWtimeout = 5 * 60 * 1000 + 456;

    function isTransientTransactionError(res) {
        return res.hasOwnProperty('errorLabels') &&
            res.errorLabels.includes('TransientTransactionError');
    }

    function isRetryingOnNetworkOrTransientTransactionError() {
        return TestData.retryingOnNetworkError || retryingOnTransientTransactionError;
    }

    function logFailedCommandAndError(cmdObj, cmdName, res) {
        if (cmdObj !== lastLoggedOp) {
            try {
                jsTestLog("Failed on cmd: " + tojson(cmdObj) + " with error: " + tojson(res));
            } catch (e) {
                jsTestLog("Failed on cmd: " + cmdName + " with error: " + tojson(res));
            }
            lastLoggedOp = cmdObj;

            if (transientErrorToLog) {
                jsTestLog("Error that caused retry of transaction " + tojson(transientErrorToLog));
            }
        }
    }

    function getTxnOptionsForClient(conn) {
        // We tack transaction options onto the client since we use one session per client.
        if (!conn.hasOwnProperty('txnOverrideOptions')) {
            conn.txnOverrideOptions = {
                stmtId: new NumberInt(0),
                autocommit: false,
                txnNumber: new NumberLong(-1),
            };
            conn.txnOverrideState = TransactionStates.kInactive;
        }
        return conn.txnOverrideOptions;
    }

    function incrementStmtIdBy(cmdName, cmdObjUnwrapped) {
        // Reserve the statement ids for batch writes.
        try {
            switch (cmdName) {
                case "insert":
                    return cmdObjUnwrapped.documents.length;
                case "update":
                    return cmdObjUnwrapped.updates.length;
                case "delete":
                    return cmdObjUnwrapped.deletes.length;
                default:
                    return 1;
            }
        } catch (e) {
            // Malformed command objects can cause errors to be thrown.
            return 1;
        }
    }

    function appendReadAndWriteConcern(conn, dbName, commandName, commandObj) {
        if (isRetryingOnNetworkOrTransientTransactionError()) {
            return;
        }

        let shouldForceReadConcern = kCommandsSupportingReadConcern.has(commandName);
        let shouldForceWriteConcern = kCommandsSupportingWriteConcern.has(commandName);

        if (commandObj.hasOwnProperty("autocommit")) {
            shouldForceReadConcern = false;
            if (commandObj.startTransaction === true) {
                shouldForceReadConcern = true;
            }
            if (!kCommandsSupportingWriteConcernInTransaction.has(commandName)) {
                shouldForceWriteConcern = false;
            }
        } else if (commandName === "aggregate") {
            if (OverrideHelpers.isAggregationWithListLocalSessionsStage(commandName, commandObj)) {
                // The $listLocalSessions stage can only be used with readConcern={level:
                // "local"}.
                shouldForceReadConcern = false;
            }

            if (OverrideHelpers.isAggregationWithOutStage(commandName, commandObj)) {
                // The $out stage can only be used with readConcern={level: "local"}.
                shouldForceReadConcern = false;
            } else {
                // A writeConcern can only be used with a $out stage.
                shouldForceWriteConcern = false;
            }

            if (commandObj.explain) {
                // Attempting to specify a readConcern while explaining an aggregation would
                // always return an error prior to SERVER-30582 and it is otherwise only
                // compatible with readConcern={level: "local"}.
                shouldForceReadConcern = false;
            }
        } else if (OverrideHelpers.isMapReduceWithInlineOutput(commandName, commandObj)) {
            // A writeConcern can only be used with non-inline output.
            shouldForceWriteConcern = false;
        }

        if (shouldForceReadConcern) {
            let readConcernLevel;
            if (commandObj.startTransaction === true) {
                readConcernLevel = kDefaultTransactionReadConcernLevel;
            } else {
                readConcernLevel = kDefaultReadConcernLevel;
            }

            if (commandObj.hasOwnProperty("readConcern") &&
                commandObj.readConcern.hasOwnProperty("level") &&
                commandObj.readConcern.level !== readConcernLevel) {
                throw new Error("refusing to override existing readConcern " +
                                commandObj.readConcern.level + " with readConcern " +
                                readConcernLevel);
            } else {
                commandObj.readConcern = {level: readConcernLevel};
            }

            // Only attach afterClusterTime if causal consistency is explicitly enabled. Note, it is
            // OK to send a readConcern with only afterClusterTime, which is interpreted as local
            // read concern by the server.
            if (TestData.hasOwnProperty("sessionOptions") &&
                TestData.sessionOptions.causalConsistency === true) {
                const driverSession = conn.getDB(dbName).getSession();
                const operationTime = driverSession.getOperationTime();
                if (operationTime !== undefined) {
                    // The command object should always have a readConcern by this point.
                    commandObj.readConcern.afterClusterTime = operationTime;
                }
            }
        }

        if (shouldForceWriteConcern) {
            if (commandObj.hasOwnProperty("writeConcern")) {
                let writeConcern = commandObj.writeConcern;
                if (typeof writeConcern !== "object" || writeConcern === null ||
                    (writeConcern.hasOwnProperty("w") &&
                     bsonWoCompare({_: writeConcern.w}, {_: kDefaultWriteConcernW}) !== 0)) {
                    throw new Error("Cowardly refusing to override write concern of command: " +
                                    tojson(commandObj));
                }
            }

            if (kCommandsSupportingWriteConcernInTransaction.has(commandName)) {
                commandObj.writeConcern = {
                    w: kDefaultTransactionWriteConcernW,
                    wtimeout: kDefaultWtimeout
                };
            } else {
                commandObj.writeConcern = {w: kDefaultWriteConcernW, wtimeout: kDefaultWtimeout};
            }
        }
    }

    function retryOnImplicitCollectionCreationIfNeeded(
        conn, dbName, commandName, commandObj, func, makeFuncArgs, res, txnOptions) {
        if (kCmdsThatInsert.has(commandName)) {
            // If the command inserted data and is not supported in a transaction, we assume it
            // failed because the collection did not exist. We will create the collection and
            // retry the command. If the collection did exist, we'll return the original
            // response because it failed for a different reason. Tests that expect collections
            // to not exist will have to be skipped.
            if (includesErrorCode(res, ErrorCodes.OperationNotSupportedInTransaction)) {
                const createCmdRes = runCommandOriginal.call(conn,
                                                             dbName,
                                                             {
                                                               create: commandObj[commandName],
                                                               lsid: commandObj.lsid,
                                                               writeConcern: {w: 'majority'},
                                                             },
                                                             0);

                if (createCmdRes.ok !== 1) {
                    // If the error is retryable, we retry the entire transaction. Otherwise, we
                    // return the original error to the caller.
                    if (createCmdRes.code !== ErrorCodes.NamespaceExists &&
                        !RetryableWritesUtil.isRetryableCode(createCmdRes.code)) {
                        logFailedCommandAndError(commandObj, commandName, createCmdRes);
                        return res;
                    }
                } else {
                    assert.commandWorked(createCmdRes);
                }
            } else {
                // If the insert command failed for any other reason, we return the original
                // response without retrying.
                logFailedCommandAndError(commandObj, commandName, res);
                return res;
            }
            // We aborted the transaction, so we need to re-run every op in the transaction,
            // rather than just the current op.
            for (let op of ops) {
                retryOp = true;
                res = runCommandInTransactionIfNeeded(
                    conn, op.dbName, op.cmdName, op.cmdObj, func, op.makeFuncArgs);

                if (res.ok !== 1) {
                    logFailedCommandAndError(commandObj, commandName, res);
                    abortTransaction(conn, commandObj.lsid, txnOptions.txnNumber);
                    return res;
                }
            }
        }

        return res;
    }

    function updateAndGossipClusterTime(conn, dbName, commitRes, commandObj) {
        // Update the latest cluster time on the session manually after we commit so
        // that we will not read too early in the next transaction. At this point, we've
        // already run through the original processCommand path where we filled in the
        // clusterTime, so we will not update it otherwise.
        conn.getDB(dbName).getSession().processCommandResponse_forTesting(commitRes);

        // Gossip the later cluster time when we retry the command.
        if (commandObj.$clusterTime) {
            commandObj.$clusterTime = commitRes.$clusterTime;
        }
    }

    function commitTransaction(conn, lsid, txnNumber) {
        const res = conn.adminCommand({
            commitTransaction: 1,
            autocommit: false, lsid, txnNumber,
            writeConcern: {w: kDefaultWriteConcernW, wtimeout: kDefaultWtimeout},
        });
        assert.commandWorked(res);
        conn.txnOverrideState = TransactionStates.kInactive;
        ops = [];

        return res;
    }

    function abortTransaction(conn, lsid, txnNumber) {
        // If there's been an error, we abort the transaction. It doesn't matter if the
        // abort call succeeds or not.
        runCommandOriginal.call(
            conn,
            'admin',
            {
              abortTransaction: 1,
              autocommit: false, lsid, txnNumber,
              writeConcern: {w: kDefaultWriteConcernW, wtimeout: kDefaultWtimeout},
            },
            0);
        conn.txnOverrideState = TransactionStates.kInactive;
    }

    function continueTransaction(conn, txnOptions, dbName, cmdName, cmdObj, makeFuncArgs) {
        if (conn.txnOverrideState === TransactionStates.kInactive) {
            // First command in a transaction.
            txnOptions.txnNumber = new NumberLong(txnOptions.txnNumber + 1);
            conn.getDB(dbName).getSession().setTxnNumber_forTesting(txnOptions.txnNumber);
            txnOptions.stmtId = new NumberInt(0);

            cmdObj.startTransaction = true;

            conn.txnOverrideState = TransactionStates.kActive;
        }

        txnOptions.stmtId = new NumberInt(txnOptions.stmtId + incrementStmtIdBy(cmdName, cmdObj));

        cmdObj.txnNumber = txnOptions.txnNumber;
        cmdObj.stmtId = txnOptions.stmtId;
        cmdObj.autocommit = false;
        delete cmdObj.writeConcern;

        // We only want to add this op to the ops array if we have not already added it. If we're
        // retrying on a transient txn or network error, this op will already have been added. If
        // retryOp is false, this op is a write command that we are retrying thus this op has
        // already been added to the ops array.
        if (!isRetryingOnNetworkOrTransientTransactionError() && !retryOp) {
            // If the command object was created in a causally consistent session but did not
            // specify a readConcern level, it may have a readConcern object with only
            // afterClusterTime. The correct read concern options are added in
            // appendReadAndWriteConcern, so remove the readConcern before saving the operation in
            // this case.
            if (cmdObj.hasOwnProperty("readConcern")) {
                // Only remove the readConcern if it only contains afterClusterTime.
                const readConcernKeys = Object.keys(cmdObj.readConcern);
                if (readConcernKeys.length !== 1 || readConcernKeys[0] !== "afterClusterTime") {
                    throw new Error("Refusing to remove existing readConcern from command: " +
                                    tojson(cmdObj));
                }
                delete cmdObj.readConcern;
            }

            ops.push({dbName, cmdName, cmdObj, makeFuncArgs});
        }

        appendReadAndWriteConcern(conn, dbName, cmdName, cmdObj);
    }

    function runCommandInTransactionIfNeeded(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        let cmdObjUnwrapped = commandObj;
        let cmdNameUnwrapped = commandName;

        if (commandName === "query" || commandName === "$query") {
            commandObj[commandName] = Object.assign({}, cmdObjUnwrapped[commandName]);
            cmdObjUnwrapped = commandObj[commandName];
            cmdNameUnwrapped = Object.keys(cmdObjUnwrapped)[0];
        }

        const commandSupportsTransaction =
            TransactionsUtil.commandSupportsTxn(dbName, cmdNameUnwrapped, cmdObjUnwrapped);

        const txnOptions = getTxnOptionsForClient(conn);
        if (commandSupportsTransaction) {
            if (cmdNameUnwrapped === "commitTransaction") {
                appendReadAndWriteConcern(conn, dbName, cmdNameUnwrapped, cmdObjUnwrapped);
                cmdObjUnwrapped.txnNumber = txnOptions.txnNumber;
            } else {
                // If running in a suite that expects network errors, commit the transaction if
                // we've run `maxOpsInTransaction` commands as a part of this transaction to avoid
                // having to retry really long running transactions. Do not commit if the current
                // command is getMore, because getMore cannot start a transaction.
                if (TestData.retryingOnNetworkError !== undefined &&
                    !isRetryingOnNetworkOrTransientTransactionError() &&
                    (commandName !== "getMore") && (ops.length >= maxOpsInTransaction) &&
                    (conn.txnOverrideState === TransactionStates.kActive)) {
                    let commitRes =
                        commitTransaction(conn, cmdObjUnwrapped.lsid, txnOptions.txnNumber);
                    updateAndGossipClusterTime(conn, dbName, commitRes, cmdObjUnwrapped);
                }

                continueTransaction(
                    conn, txnOptions, dbName, cmdNameUnwrapped, cmdObjUnwrapped, makeFuncArgs);
                retryOp = false;
            }
        } else {
            if (conn.txnOverrideState === TransactionStates.kActive) {
                let commitRes = commitTransaction(conn, cmdObjUnwrapped.lsid, txnOptions.txnNumber);
                updateAndGossipClusterTime(conn, dbName, commitRes, cmdObjUnwrapped);
            } else {
                ops = [];
            }

            appendReadAndWriteConcern(conn, dbName, cmdNameUnwrapped, cmdObjUnwrapped);
            if (commandName === 'drop' || commandName === 'convertToCapped') {
                // Convert all collection drops to w:majority so they won't prevent subsequent
                // operations in transactions from failing when failing to acquire collection locks.
                if (!cmdObjUnwrapped.writeConcern) {
                    cmdObjUnwrapped.writeConcern = {};
                }
                cmdObjUnwrapped.writeConcern.w = 'majority';
            }
        }

        let res = func.apply(conn, makeFuncArgs(commandObj));

        if ((res.ok !== 1 || res.writeErrors) &&
            (conn.txnOverrideState === TransactionStates.kActive)) {
            abortTransaction(conn, cmdObjUnwrapped.lsid, txnOptions.txnNumber);
            res = retryOnImplicitCollectionCreationIfNeeded(conn,
                                                            dbName,
                                                            cmdNameUnwrapped,
                                                            cmdObjUnwrapped,
                                                            func,
                                                            makeFuncArgs,
                                                            res,
                                                            txnOptions);
        }

        return res;
    }

    function retryEntireTransaction(conn, lsid, func) {
        let txnOptions = getTxnOptionsForClient(conn);
        let txnNumber = txnOptions.txnNumber;
        jsTestLog("Retrying entire transaction for aborted txn with txnNum: " + txnNumber +
                  " and lsid " + tojson(lsid));
        // Set the transactionState to inactive so continueTransaction() will bump the
        // txnNum.
        conn.txnOverrideState = TransactionStates.kInactive;

        // Re-run every command in the ops array.
        assert.gt(ops.length, 0);

        let res;
        for (let op of ops) {
            res = runCommandInTransactionIfNeeded(
                conn, op.dbName, op.cmdName, op.cmdObj, func, op.makeFuncArgs);

            if (isTransientTransactionError(res)) {
                return retryEntireTransaction(conn, lsid, func);
            }
        }

        return res;
    }

    function retryCommitTransaction(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        let res;
        let retryCommit = false;
        jsTestLog("Retrying commitTransaction for txnNum: " + commandObj.txnNumber + " and lsid: " +
                  tojson(commandObj.lsid));
        do {
            res = runCommandInTransactionIfNeeded(
                conn, dbName, "commitTransaction", commandObj, func, makeFuncArgs);

            if (res.writeConcernError) {
                retryCommit = true;
                continue;
            }

            if (isTransientTransactionError(res)) {
                transientErrorToLog = res;
                retryCommit = true;
                res = retryEntireTransaction(conn, commandObj.lsid, func);
            } else if (res.ok === 1) {
                retryCommit = false;
            }
        } while (retryCommit);

        return res;
    }

    function runCommandOnNetworkOrTransientTransactionErrorRetry(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        jsTestLog("Retrying command on network error or TransientTransactionError: " +
                  tojsononeline(commandObj));
        transientErrorToLog = null;
        // If the ops array is empty, we failed on a command not being run in a
        // transaction and need to retry just this command.
        if (ops.length === 0) {
            // Set the transactionState to inactive so continueTransaction() will bump the
            // txnNum.
            conn.txnOverrideState = TransactionStates.kInactive;
            return runCommandInTransactionIfNeeded(
                conn, dbName, commandName, commandObj, func, makeFuncArgs);
        }

        if (commandName === "commitTransaction") {
            return retryCommitTransaction(
                conn, dbName, commandName, commandObj, func, makeFuncArgs);
        }

        return retryEntireTransaction(conn, commandObj.lsid, func);
    }

    function runCommandWithTransactionRetries(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        const driverSession = conn.getDB(dbName).getSession();
        if (driverSession.getSessionId() === null) {
            // Sessions is explicitly disabled for this command. So we skip overriding it to
            // use transactions.
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        let res;
        if (!isRetryingOnNetworkOrTransientTransactionError()) {
            res = runCommandInTransactionIfNeeded(
                conn, dbName, commandName, commandObj, func, makeFuncArgs);

            if (commandName === "commitTransaction") {
                while (res.writeConcernError) {
                    jsTestLog("Retrying commitTransaction on WCE for txnNum: " +
                              commandObj.txnNumber + " and lsid: " + tojson(commandObj.lsid));
                    res = runCommandInTransactionIfNeeded(
                        conn, dbName, commandName, commandObj, func, makeFuncArgs);
                }
            }

            return res;
        }

        res = runCommandOnNetworkOrTransientTransactionErrorRetry(
            conn, dbName, commandName, commandObj, func, makeFuncArgs);

        return res;
    }

    function runCommandWithTransientTransactionErrorRetries(
        conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        retryingOnTransientTransactionError = false;
        while (true) {
            try {
                let res = runCommandWithTransactionRetries(
                    conn, dbName, commandName, commandObj, func, makeFuncArgs);

                if (isTransientTransactionError(res)) {
                    retryingOnTransientTransactionError = true;
                    print("=-=-=-= Retrying on TransientTransactionError for command: " +
                          commandName + ", response: " + tojson(res));
                    continue;
                }

                retryingOnTransientTransactionError = false;
                return res;
            } catch (e) {
                if (isTransientTransactionError(e)) {
                    retryingOnTransientTransactionError = true;
                    print("=-=-=-= Retrying on TransientTransactionError for command: " +
                          commandName + ", error: " + tojson(e));
                    continue;
                }

                retryingOnTransientTransactionError = false;
                throw e;
            }
        }
    }

    startParallelShell = function() {
        throw new Error(
            "Cowardly refusing to run test with transaction override enabled when it uses" +
            "startParalleShell()");
    };

    OverrideHelpers.overrideRunCommand(runCommandWithTransientTransactionErrorRetries);
})();
