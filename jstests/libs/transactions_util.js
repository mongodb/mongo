/**
 * Utilities for testing transactions.
 */
var TransactionsUtil = (function() {
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

    function commandSupportsTxn(dbName, cmdName, cmdObj) {
        if (cmdName === 'commitTransaction' || cmdName === 'abortTransaction') {
            return true;
        }

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

    return {
        commandSupportsTxn, commandTypeCanSupportTxn,
    };
})();
