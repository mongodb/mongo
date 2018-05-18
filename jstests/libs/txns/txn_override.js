/**
 * Override to run consecutive operations inside the same transaction. When an operation that
 * cannot be run inside of a transaction is encountered, the active transaction is committed
 * before running the next operation.
 */

(function() {
    'use strict';

    load('jstests/libs/override_methods/override_helpers.js');

    const runCommandOriginal = Mongo.prototype.runCommand;

    const kCmdsSupportingTransactions = new Set([
        'aggregate',
        'delete',
        'find',
        'findAndModify',
        'findandmodify',
        'getMore',
        'insert',
        'update',
    ]);

    const kCmdsThatWrite = new Set([
        'insert',
        'update',
        'findAndModify',
        'findandmodify',
        'delete',
    ]);

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

    function commandSupportsTxn(dbName, cmdName, cmdObj) {
        if (!kCmdsSupportingTransactions.has(cmdName)) {
            return false;
        }

        if (dbName === 'local' || dbName === 'config' || dbName === 'admin') {
            return false;
        }

        if (kCmdsThatWrite.has(cmdName)) {
            if (cmdObj[cmdName].startsWith('system.')) {
                return false;
            }
        }
        return true;
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

    function commitTransaction(conn, lsid, txnNumber) {
        const res = runCommandOriginal.call(conn,
                                            'admin',
                                            {
                                              commitTransaction: 1,
                                              autocommit: false, lsid, txnNumber,
                                            },
                                            0);
        assert.commandWorked(res);
        conn.txnOverrideState = TransactionStates.kInactive;
    }

    function abortTransaction(conn, lsid, txnNumber) {
        // If there's been an error, we abort the transaction. It doesn't matter if the
        // abort call succeeds or not.
        runCommandOriginal.call(conn,
                                'admin',
                                {
                                  abortTransaction: 1,
                                  autocommit: false,
                                  lsid: lsid,
                                  txnNumber: txnNumber,
                                },
                                0);

        conn.txnOverrideState = TransactionStates.kInactive;
    }

    function continueTransaction(conn, txnOptions, cmdName, cmdObj) {
        if (conn.txnOverrideState === TransactionStates.kInactive) {
            // First command in a transaction.
            txnOptions.txnNumber = new NumberLong(txnOptions.txnNumber + 1);
            txnOptions.stmtId = new NumberInt(0);

            cmdObj.startTransaction = true;

            if (cmdObj.readConcern && cmdObj.readConcern.level !== 'snapshot') {
                throw new Error("refusing to override existing readConcern");
            } else {
                cmdObj.readConcern = {level: 'snapshot'};
            }

            conn.txnOverrideState = TransactionStates.kActive;
        }

        txnOptions.stmtId = new NumberInt(txnOptions.stmtId + incrementStmtIdBy(cmdName, cmdObj));

        cmdObj.txnNumber = txnOptions.txnNumber;
        cmdObj.stmtId = txnOptions.stmtId;
        cmdObj.autocommit = false;
    }

    function runCommandWithTransactions(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        const driverSession = conn.getDB(dbName).getSession();
        if (driverSession.getSessionId() === null) {
            // Sessions is explicitly disabled for this command. So we skip overriding it to
            // use transactions.
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        let cmdObjUnwrapped = commandObj;
        let cmdNameUnwrapped = commandName;

        if (commandName === "query" || commandName === "$query") {
            commandObj[commandName] = Object.assign({}, cmdObjUnwrapped[commandName]);
            cmdObjUnwrapped = commandObj[commandName];
            cmdNameUnwrapped = Object.keys(cmdObjUnwrapped)[0];
        }

        const commandSupportsTransaction =
            commandSupportsTxn(dbName, cmdNameUnwrapped, cmdObjUnwrapped);

        const txnOptions = getTxnOptionsForClient(conn);

        if (!commandSupportsTransaction) {
            if (conn.txnOverrideState === TransactionStates.kActive) {
                commitTransaction(conn, commandObj.lsid, txnOptions.txnNumber);
            }

        } else {
            continueTransaction(conn, txnOptions, cmdNameUnwrapped, cmdObjUnwrapped);
        }

        if (commandName === 'drop' || commandName === 'convertToCapped') {
            // Convert all collection drops to w:majority so they won't prevent subsequent
            // operations in transactions from failing when failing to acquire collection locks.
            if (!cmdObjUnwrapped.writeConcern) {
                cmdObjUnwrapped.writeConcern = {};
            }
            cmdObjUnwrapped.writeConcern.w = 'majority';
        }

        let res = func.apply(conn, makeFuncArgs(commandObj));

        if (res.ok !== 1) {
            abortTransaction(conn, commandObj.lsid, txnOptions.txnNumber);
            if (kCmdsThatInsert.has(cmdNameUnwrapped)) {
                // If the command inserted data, we check if it failed because the collection did
                // not exist; if so, create the collection and retry the command. Tests that
                // expect collections to not exist will have to be skipped.
                if (res.code === ErrorCodes.NamespaceNotFound) {
                    const createCmdRes =
                        runCommandOriginal.call(conn,
                                                dbName,
                                                {
                                                  create: cmdObjUnwrapped[cmdNameUnwrapped],
                                                  lsid: commandObj.lsid,
                                                  writeConcern: {w: 'majority'},
                                                },
                                                0);

                    if (createCmdRes.ok !== 1) {
                        if (createCmdRes.code !== ErrorCodes.NamespaceExists) {
                            // The collection still does not exist. So we just return the original
                            // response to the caller,
                            return res;
                        }
                    } else {
                        assert.commandWorked(createCmdRes);
                    }
                } else {
                    // If the insert command failed for any other reason, we return the original
                    // response without retrying.
                    return res;
                }

                continueTransaction(conn, txnOptions, cmdNameUnwrapped, cmdObjUnwrapped);

                res = func.apply(conn, makeFuncArgs(commandObj));
                if (res.ok !== 1) {
                    abortTransaction(conn, commandObj.lsid, txnOptions.txnNumber);
                }
            }
        }

        return res;
    }

    startParallelShell = function() {
        throw new Error(
            "Cowardly refusing to run test with transaction override enabled when it uses" +
            "startParalleShell()");
    };

    OverrideHelpers.overrideRunCommand(runCommandWithTransactions);
})();