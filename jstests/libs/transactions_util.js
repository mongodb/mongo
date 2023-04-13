/**
 * Utilities for testing transactions.
 */
var TransactionsUtil = (function() {
    load("jstests/libs/override_methods/override_helpers.js");

    // Although createCollection and createIndexes are supported inside multi-document
    // transactions, we intentionally exclude them from this list since they are non-
    // idempotent and, for createIndexes, are not supported inside multi-document
    // transactions for all cases.
    const kCmdsSupportingTransactions = new Set([
        'aggregate',
        'delete',
        'find',
        'findAndModify',
        'findandmodify',
        'getMore',
        'insert',
        'update',
        'bulkWrite',
    ]);

    const kCmdsThatWrite = new Set([
        'insert',
        'update',
        'findAndModify',
        'findandmodify',
        'delete',
        'bulkWrite',
    ]);

    // Indicates an aggregation command with a pipeline that cannot run in a transaction but can
    // still execute concurrently with other transactions. Pipelines with $changeStream or $out
    // cannot run within a transaction.
    function commandIsNonTxnAggregation(cmdName, cmdObj) {
        return OverrideHelpers.isAggregationWithOutOrMergeStage(cmdName, cmdObj) ||
            OverrideHelpers.isAggregationWithChangeStreamStage(cmdName, cmdObj);
    }

    function commandSupportsTxn(dbName, cmdName, cmdObj) {
        if (cmdName === 'commitTransaction' || cmdName === 'abortTransaction') {
            return true;
        }

        if (!kCmdsSupportingTransactions.has(cmdName) ||
            commandIsNonTxnAggregation(cmdName, cmdObj)) {
            return false;
        }

        // bulkWrite always operates on the admin DB so cannot check the dbName directly.
        // Operating namespaces are also contained within a 'nsInfo' array in the command.
        if (cmdName === 'bulkWrite') {
            // 'nsInfo' does not exist in command.
            if (!cmdObj['nsInfo']) {
                return false;
            }

            // Loop through 'nsInfo'.
            for (const ns of cmdObj['nsInfo']) {
                if (!ns['ns']) {
                    return false;
                }
                var db = ns['ns'].split('.', 1)[0];
                if (db === 'local' || db === 'config' || db === 'system') {
                    return false;
                }
            }
        } else {
            if (dbName === 'local' || dbName === 'config' || dbName === 'admin') {
                return false;
            }

            if (kCmdsThatWrite.has(cmdName)) {
                if (cmdObj[cmdName].startsWith('system.')) {
                    return false;
                }
            }
        }

        if (cmdObj.lsid === undefined) {
            return false;
        }

        return true;
    }

    function commandTypeCanSupportTxn(cmdName) {
        if (cmdName === 'commitTransaction' || cmdName === 'abortTransaction') {
            return true;
        }

        if (kCmdsSupportingTransactions.has(cmdName)) {
            return true;
        }
        return false;
    }

    // Make a deep copy of an object for retrying transactions. We make deep copies of object and
    // array literals but not custom types like DB and DBCollection because they could have been
    // modified before a transaction aborts. This function is adapted from the implementation of
    // Object.extend() in src/mongo/shell/types.js.
    function deepCopyObject(dst, src) {
        for (var k in src) {
            var v = src[k];
            if (typeof (v) == "object" && v !== null) {
                if (v.constructor === ObjectId) {  // convert ObjectId properly
                    eval("v = " + tojson(v));
                } else if (v instanceof NumberLong) {  // convert NumberLong properly
                    eval("v = " + tojson(v));
                } else if (v instanceof Date) {  // convert Date properly
                    eval("v = " + tojson(v));
                } else if (v instanceof Timestamp) {  // convert Timestamp properly
                    eval("v = " + tojson(v));
                } else if (Object.getPrototypeOf(v) === Object.prototype) {
                    v = deepCopyObject({}, v);
                } else if (Array.isArray(v)) {
                    v = deepCopyObject([], v);
                }
            }
            var desc = Object.getOwnPropertyDescriptor(src, k);
            desc.value = v;
            Object.defineProperty(dst, k, desc);
        }
        return dst;
    }

    function isTransientTransactionError(res) {
        return res.hasOwnProperty('errorLabels') &&
            res.errorLabels.includes('TransientTransactionError');
    }

    // Runs a function 'func()' in a transaction on database 'db'. Invokes function
    // 'beforeTransactionFunc()' before the transaction (can be used to get references to
    // collections etc.). Ensures that the transaction is successfully committed, by retrying the
    // function 'func' in case of the transaction being aborted until the timeout is hit.
    //
    // Function 'beforeTransactionFunc(db, session)' accepts database in session 'db' and the
    // session 'session'.
    // Function 'func(db, state)' accepts database in session 'db' and an object returned by
    // 'beforeTransactionFunc()' - 'state'.
    // 'transactionOptions' - parameters for the transaction.
    function runInTransaction(db, beforeTransactionFunc, func, transactionOptions = {}) {
        const session = db.getMongo().startSession();
        const sessionDb = session.getDatabase(db.getName());
        const state = beforeTransactionFunc(sessionDb, session);
        let commandResponse;
        assert.soon(() => {
            session.startTransaction(transactionOptions);
            func(sessionDb, state);
            try {
                commandResponse = assert.commandWorked(session.commitTransaction_forTesting());
                return true;
            } catch {
                return false;
            }
        });
        return commandResponse;
    }

    return {
        commandIsNonTxnAggregation,
        commandSupportsTxn,
        commandTypeCanSupportTxn,
        deepCopyObject,
        isTransientTransactionError,
        runInTransaction,
    };
})();
