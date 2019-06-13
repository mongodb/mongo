/**
 * Utilities for testing transactions.
 */
var TransactionsUtil = (function() {
    load("jstests/libs/override_methods/override_helpers.js");

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

    // Make a deep copy of an object for retrying transactions. We make deep copies of object and
    // array literals but not custom types like DB and DBCollection because they could have been
    // modified before a transaction aborts. This function is adapted from the implementation of
    // Object.extend() in src/mongo/shell/types.js.
    function deepCopyObject(dst, src) {
        for (var k in src) {
            var v = src[k];
            if (typeof(v) == "object" && v !== null) {
                if (v.constructor === ObjectId) {  // convert ObjectId properly
                    eval("v = " + tojson(v));
                } else if (v instanceof NumberLong) {  // convert NumberLong properly
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

    return {
        commandIsNonTxnAggregation,
        commandSupportsTxn,
        commandTypeCanSupportTxn,
        deepCopyObject,
        isTransientTransactionError,
    };
})();
