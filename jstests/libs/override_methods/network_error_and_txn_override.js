/**
 * This override (1) wraps consecutive CRUD operations in transactions and (2) retries commands and
 * transactions on network errors. Both behaviors are only enabled with a TestData config parameter.
 *
 * (1) Consecutive CRUD operations that are supported in transactions are wrapped in transactions.
 * When an operation that cannot be run in a transaction is encountered, the active transaction is
 * committed before running the next operation. The override retries TransientTransactionErrors,
 * which are expected without failover in sharding, and automatically creates collections and
 * retries transactions that fail due to implicit collection creation.
 *
 * (2) When a network connection to the mongo shell is closed, attempting to call
 * Mongo.prototype.runCommand() throws a JavaScript exception. This override catches these
 * exceptions (i.e. ones where isNetworkError() returns true) and automatically re-sends the
 * command request to the server, or propagates the error if the command should already be using
 * the shell's existing retryability logic. The goal of this override is to implement retry logic
 * such that the assertions within our existing JavaScript tests still pass despite stepdowns of
 * replica set primaries (optionally in sharded clusters) happening in the background.
 *
 * These two overrides are unified to simplify the retry logic.
 *
 * Unittests for these overrides are included in:
 *     jstests/noPassthrough/txn_override_causal_consistency.js
 *     jstests/replsets/txn_override_unittests.js
 *     jstests/libs/txns/txn_passthrough_runner_selftest.js
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    kCommandsSupportingReadConcern,
    kCommandsSupportingWriteConcern,
    kCommandsSupportingWriteConcernInTransaction
} from "jstests/libs/override_methods/read_and_write_concern_helpers.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {TransactionsUtil} from "jstests/libs/transactions_util.js";

// Truncates the 'print' output if it's too long to print.
const kMaxPrintLength = 5000;
const kNumPrintEndChars = kMaxPrintLength / 2;
const originalPrint = print;
globalThis.print = function(msg) {
    if (typeof msg !== "string") {
        originalPrint(msg);
        return;
    }

    const len = msg.length;
    if (len <= kMaxPrintLength) {
        originalPrint(msg);
        return;
    }

    originalPrint(`${msg.substr(0, kNumPrintEndChars)}...${msg.substr(len - kNumPrintEndChars)}`);
};

function configuredForNetworkRetry() {
    assert(TestData.networkErrorAndTxnOverrideConfig, TestData);
    return TestData.networkErrorAndTxnOverrideConfig.retryOnNetworkErrors &&
        !jsTest.options().skipRetryOnNetworkError;
}

function configuredForTxnOverride() {
    assert(TestData.networkErrorAndTxnOverrideConfig, TestData);
    return TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions;
}

function configuredForBackgroundReconfigs() {
    assert(TestData.networkErrorAndTxnOverrideConfig, TestData);
    return TestData.networkErrorAndTxnOverrideConfig.backgroundReconfigs;
}

// Commands assumed to not be blindly retryable.
const kNonRetryableCommands = new Set([
    // Commands that take write concern and do not support txnNumbers.
    "_configsvrAddShard",
    "_configsvrAddShardToZone",
    "_configsvrCommitChunksMerge",
    "_configsvrCommitChunkMigration",
    "_configsvrCommitChunkSplit",
    "_configsvrCommitMergeAllChunksOnShard",
    "_configsvrCreateDatabase",
    "_configsvrMoveRange",
    "_configsvrRemoveShard",
    "_configsvrRemoveShardFromZone",
    "_configsvrUpdateZoneKeyRange",
    "_mergeAuthzCollections",
    "_recvChunkStart",
    "appendOplogNote",
    "applyOps",
    "captrunc",
    "clone",
    "cloneCollectionAsCapped",
    "create",
    "createIndexes",
    "deleteIndexes",
    "drop",
    "dropAllRolesFromDatabase",
    "dropAllUsersFromDatabase",
    "dropDatabase",
    "dropIndexes",
    "dropRole",
    "dropUser",
    "emptycapped",
    "godinsert",
    "internalRenameIfOptionsAndIndexesMatch",
    "updateRole",
    "updateUser",
]);

// These commands are not idempotent because they return errors if retried after successfully
// completing (like IndexNotFound, NamespaceExists, etc.), but because they only take effect
// once, and many tests use them to set up state, their errors on retries are handled specially.
const kAcceptableNonRetryableCommands = new Set([
    "create",
    "createIndexes",
    "createRole",
    "createUser",
    "deleteIndexes",
    "drop",
    "dropDatabase",  // Already ignores NamespaceNotFound errors, so not handled below.
    "dropIndexes",
    "moveChunk",
]);

// The following read operations defined in the CRUD specification are retryable.
// Note that estimatedDocumentCount() and countDocuments() use the count command.
const kRetryableReadCommands = new Set(["find", "aggregate", "distinct", "count"]);

// Returns true if the command name is that of a retryable read command.
function isRetryableReadCmdName(cmdName) {
    return kRetryableReadCommands.has(cmdName);
}

// Returns if the given failed response is a safe response to ignore when retrying the
// given command type.
function isAcceptableRetryFailedResponse(cmdName, res) {
    assert(!res.ok, res);
    // These codes are uniquely returned from user_management_commands.cpp
    const kErrorCodeRoleAlreadyExists = 51002;
    const kErrorCodeUserAlreadyExists = 51003;
    return ((cmdName === "create" && res.code === ErrorCodes.NamespaceExists) ||
            (cmdName === "createIndexes" && res.code === ErrorCodes.IndexAlreadyExists) ||
            (cmdName === "drop" && res.code === ErrorCodes.NamespaceNotFound) ||
            ((cmdName == "createUser") && (res.code === kErrorCodeUserAlreadyExists)) ||
            ((cmdName == "createRole") && (res.code === kErrorCodeRoleAlreadyExists)) ||
            ((cmdName === "dropIndexes" || cmdName === "deleteIndexes") &&
             res.code === ErrorCodes.IndexNotFound));
}

// Commands that may return different values or fail if retried on a new primary after a
// failover.
const kNonFailoverTolerantCommands = new Set([
    "currentOp",  // Failovers can change currentOp output.
    "getLog",     // The log is different on different servers.
    "killOp",     // Failovers may interrupt operations intended to be killed later in the test.
    "logRotate",
    "planCacheClear",  // The plan cache isn't replicated.
    "planCacheClearFilters",
    "planCacheListFilters",
    "planCacheSetFilter",
    "profile",       // Not replicated, so can't tolerate failovers.
    "setParameter",  // Not replicated, so can't tolerate failovers.
    "stageDebug",
    "startSession",  // Sessions are flushed to disk asynchronously.
]);

function isCommitOrAbort(cmdName) {
    return cmdName === "commitTransaction" || cmdName === "abortTransaction";
}

function isCmdInTransaction(cmdObj) {
    return cmdObj.hasOwnProperty("autocommit");
}

// Returns if the given command on the given database can retry network errors.
function canRetryNetworkErrorForCommand(cmdName, cmdObj) {
    if (!configuredForNetworkRetry()) {
        return false;
    }

    if (isCmdInTransaction(cmdObj)) {
        // Commands in transactions cannot be retried at the statement level, except for the
        // commit and abort.
        return isCommitOrAbort(cmdName);
    }

    return true;
}

// Returns if the given command should retry a read error when reconfigs are present.
function canRetryReadErrorDuringBackgroundReconfig(cmdName) {
    if (!configuredForBackgroundReconfigs()) {
        return false;
    }
    return isRetryableReadCmdName(cmdName);
}

// When running the reconfig command on a node, it will drop its snapshot. Read commands issued
// to this node before it updates its snapshot will fail with ReadConcernMajorityNotAvailableYet.
function isRetryableReadCode(code) {
    return code === ErrorCodes.ReadConcernMajorityNotAvailableYet;
}

// Several commands that use the plan executor swallow the actual error code from a failed plan
// into their error message and instead return OperationFailed.
//
// TODO SERVER-32208: Remove this function once it is no longer needed.
function isRetryableExecutorCodeAndMessage(code, msg) {
    return code === ErrorCodes.OperationFailed && typeof msg !== "undefined" &&
        msg.indexOf("InterruptedDueToReplStateChange") >= 0;
}

// Returns true if the given response could have come from shardCollection being interrupted by
// a failover.
function isRetryableShardCollectionResponse(res) {
    // shardCollection can bury the original error code in the error message.
    return RetryableWritesUtil.errmsgContainsRetryableCodeName(res.errmsg) ||
        // shardCollection creates collections on each shard that will receive a chunk using
        // _cloneCollectionsOptionsFromPrimaryShard, which may fail with the following code if
        // interupted by a failover.
        res.code === ErrorCodes.CallbackCanceled;
}

// Returns true if the given response could have come from moveChunk being interrupted by a
// failover.
function isRetryableMoveChunkResponse(res) {
    return (res.code === ErrorCodes.OperationFailed &&
            (RetryableWritesUtil.errmsgContainsRetryableCodeName(res.errmsg) ||
             // The transaction number is bumped by the migration coordinator when its commit or
             // abort decision is being made durable.
             res.errmsg.includes("TransactionTooOld") ||
             // The range deletion task may have been interrupted. This error can occur even when
             // _waitForDelete=false.
             res.errmsg.includes("operation was interrupted"))) ||
        // This error may occur when the node is shutting down.
        res.code === ErrorCodes.CallbackCanceled;
}

function hasError(res) {
    return res.ok !== 1 || res.writeErrors || (res.hasOwnProperty("nErrors") && res.nErrors != 0);
}

function hasWriteConcernError(res) {
    return res.hasOwnProperty("writeConcernError");
}

// Tracks if the current command is being run in a network retry. This is specifically for
// retries that this file initiates, not ones that retryable writes initiates.
let inCommandNetworkErrorRetry = false;

// "Command ID" is an identifier for a given command being overridden. This is to track what log
// messages come from what commands. This override is highly recursive and this is helpful for
// debugging that recursion and following what commands initiated other commands.
let currentCommandID = [];
let newestCommandID = 0;

// The "nesting level" specifies if this is a top level command or a command being recursively
// run by the override itself.
let nestingLevel = 0;
function isNested() {
    assert.gt(nestingLevel, 0);
    return nestingLevel !== 1;
}

// An object that tracks the current stmtId and txnNumber of the most recently run transaction.
let txnOptions = {
    stmtId: new NumberInt(0),
    txnNumber: new NumberLong(-1),
};

// Array to hold pairs of (dbName, cmdObj) that will be iterated over when retrying an entire
// transaction.
let ops = [];
function clearOpsList() {
    ops = [];
}

// The (initially empty) set of cursors belonging to aggregation operations that executed
// outside of a transaction. Any getMore operations on these cursors must also execute outside
// of a transaction. The set stores key/value pairs where the key is a cursor id and the value
// is the true boolean value.
let nonTxnAggCursorSet = {};

// Set the max number of operations to run in a transaction. Once we've hit this number of
// operations, we will commit the transaction. This is to prevent having to retry an extremely
// long running transaction.
const maxOpsInTransaction = 10;

const kLogPrefix = "=-=-=-=";

function logErrorFull(msg, cmdName, cmdObj, res) {
    print(`${kLogPrefix} ${msg} :: ${cmdName}, CommandID: ${currentCommandID},` +
          ` error: ${tojsononeline(res)}, command: ${tojsononeline(cmdObj)}`);
    assert.eq(nestingLevel, currentCommandID.length);
}

function logMsgFull(msgHeader, msgFooter) {
    print(`${kLogPrefix} ${msgHeader} :: CommandID: ${currentCommandID}, msg: ${msgFooter}`);
    assert.eq(nestingLevel, currentCommandID.length);
}

// Validate the command before running it, to prevent tests with non-retryable commands
// from being run.
function validateCmdNetworkErrorCompatibility(cmdName, cmdObj) {
    assert(!inCommandNetworkErrorRetry);
    assert(!isNested());

    const isRetryableWriteCmd = RetryableWritesUtil.isRetryableWriteCmdName(cmdName);
    const canRetryWrites = _ServerSession.canRetryWrites(cmdObj);
    const logSuffix = " CmdName: " + cmdName + ", CmdObj: " + tojson(cmdObj);

    if (isRetryableWriteCmd && !canRetryWrites) {
        throw new Error("Refusing to run a test that issues non-retryable write operations" +
                        " since the test likely makes assertions on the write results and" +
                        " can lead to spurious failures if a network error occurs." + logSuffix);
    } else if (cmdName === "getMore") {
        throw new Error(
            "Refusing to run a test that issues a getMore command since if a network error" +
            " occurs during it then we won't know whether the cursor was advanced or not." +
            logSuffix);
    } else if (kNonRetryableCommands.has(cmdName) &&
               !kAcceptableNonRetryableCommands.has(cmdName)) {
        throw new Error(
            "Refusing to run a test that issues commands that are not blindly retryable, " +
            logSuffix);
    } else if (kNonFailoverTolerantCommands.has(cmdName)) {
        throw new Error(
            "Refusing to run a test that issues commands that may return different values" +
            " after a failover, " + logSuffix);
    } else if (cmdName === "aggregate") {
        var stages = cmdObj.pipeline;

        // $listLocalSessions must be the first stage in the pipeline.
        const firstStage =
            stages && Array.isArray(stages) && (stages.length > 0) ? stages[0] : undefined;
        const hasListLocalStage = firstStage && (typeof firstStage === "object") &&
            firstStage.hasOwnProperty("$listLocalSessions");
        if (hasListLocalStage) {
            throw new Error("Refusing to run a test that issues an aggregation command with" +
                            " $listLocalSessions because it relies on in-memory" +
                            " state that may not survive failovers." + logSuffix);
        }

        // Aggregate can be either a read or a write depending on whether it has a $out stage.
        // $out is required to be the last stage of the pipeline.
        const lastStage = stages && Array.isArray(stages) && (stages.length !== 0)
            ? stages[stages.length - 1]
            : undefined;
        const hasOut =
            lastStage && (typeof lastStage === "object") && lastStage.hasOwnProperty("$out");
        if (hasOut) {
            throw new Error("Refusing to run a test that issues an aggregation command" +
                            " with $out because it is not retryable." + logSuffix);
        }

        const hasExplain = cmdObj.hasOwnProperty("explain");
        if (hasExplain) {
            throw new Error(
                "Refusing to run a test that issues an aggregation command with explain" +
                " because it may return incomplete results if interrupted by a stepdown." +
                logSuffix);
        }
    }
}

// Default read concern level to use for transactions. Snapshot read concern is not supported in
// sharded transactions when majority reads are disabled.
const kDefaultTransactionReadConcernLevel =
    TestData.hasOwnProperty("defaultTransactionReadConcernLevel")
    ? TestData.defaultTransactionReadConcernLevel
    : (TestData.enableMajorityReadConcern !== false ? "snapshot" : "local");

const kDefaultTransactionWriteConcernW = TestData.hasOwnProperty("defaultTransactionWriteConcernW")
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
const kDefaultWriteConcernW =
    TestData.hasOwnProperty("defaultWriteConcernW") ? TestData.defaultWriteConcernW : "majority";

// Use a "signature" value that won't typically match a value assigned in normal use. This way
// the wtimeout set by this override is distinguishable in the server logs.
const kDefaultWtimeout = 5 * 60 * 1000 + 567;

function appendReadAndWriteConcern(conn, dbName, cmdName, cmdObj) {
    let shouldForceReadConcern = kCommandsSupportingReadConcern.has(cmdName);
    let shouldForceWriteConcern = kCommandsSupportingWriteConcern.has(cmdName);

    if (isCmdInTransaction(cmdObj)) {
        shouldForceReadConcern = false;
        if (cmdObj.startTransaction === true) {
            shouldForceReadConcern = true;
        }
        if (!kCommandsSupportingWriteConcernInTransaction.has(cmdName)) {
            shouldForceWriteConcern = false;
        }
    } else if (cmdName === "aggregate") {
        if (OverrideHelpers.isAggregationWithListLocalSessionsStage(cmdName, cmdObj) ||
            OverrideHelpers.isAggregationWithChangeStreamStage(cmdName, cmdObj) ||
            OverrideHelpers.isAggregationWithCurrentOpStage(cmdName, cmdObj)) {
            // The $listLocalSessions and $currentOp stages can only be used with
            // readConcern={level: "local"}, and the $changeStream stage can only be used with
            // readConcern={level: "majority"}.
            shouldForceReadConcern = false;
        }

        if (OverrideHelpers.isAggregationWithOutOrMergeStage(cmdName, cmdObj)) {
            // The $out stage can only be used with readConcern={level: "local"}.
            shouldForceReadConcern = false;
        } else {
            // A writeConcern can only be used with a $out stage.
            shouldForceWriteConcern = false;
        }

        if (cmdObj.explain) {
            // Attempting to specify a readConcern while explaining an aggregation would always
            // return an error prior to SERVER-30582 and it is otherwise only compatible with
            // readConcern={level: "local"}.
            shouldForceReadConcern = false;
        }
    } else if (OverrideHelpers.isMapReduceWithInlineOutput(cmdName, cmdObj)) {
        // A writeConcern can only be used with non-inline output.
        shouldForceWriteConcern = false;
    }

    // If we're retrying on network errors the write concern should already be majority.
    if ((cmdName === 'drop' || cmdName === 'convertToCapped') && configuredForTxnOverride() &&
        !configuredForNetworkRetry()) {
        // Convert all collection drops to w:majority so they won't prevent subsequent
        // operations in transactions from failing when failing to acquire collection locks.
        cmdObj.writeConcern = cmdObj.writeConcern || {w: "majority", wtimeout: kDefaultWtimeout};
        shouldForceWriteConcern = false;
    }

    if (shouldForceReadConcern) {
        let readConcernLevel;
        if (cmdObj.startTransaction === true) {
            readConcernLevel = kDefaultTransactionReadConcernLevel;
        } else {
            readConcernLevel = kDefaultReadConcernLevel;
        }

        if (cmdObj.hasOwnProperty("readConcern") && cmdObj.readConcern.hasOwnProperty("level") &&
            cmdObj.readConcern.level !== readConcernLevel) {
            throw new Error("refusing to override existing readConcern " +
                            cmdObj.readConcern.level + " with readConcern " + readConcernLevel);
        } else {
            cmdObj.readConcern = {level: readConcernLevel};
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
                cmdObj.readConcern.afterClusterTime = operationTime;
            }
        }
    }

    if (shouldForceWriteConcern) {
        if (cmdObj.hasOwnProperty("writeConcern")) {
            let writeConcern = cmdObj.writeConcern;
            if (typeof writeConcern !== "object" || writeConcern === null ||
                (writeConcern.hasOwnProperty("w") &&
                 bsonWoCompare({_: writeConcern.w}, {_: kDefaultWriteConcernW}) !== 0 &&
                 bsonWoCompare({_: writeConcern.w}, {_: 1}) !== 0)) {
                throw new Error("Cowardly refusing to override write concern of command: " +
                                tojson(cmdObj));
            }
        }

        if (kCommandsSupportingWriteConcernInTransaction.has(cmdName)) {
            cmdObj.writeConcern = {w: kDefaultTransactionWriteConcernW, wtimeout: kDefaultWtimeout};
        } else {
            cmdObj.writeConcern = {w: kDefaultWriteConcernW, wtimeout: kDefaultWtimeout};
        }
    }
}

// Commits the given transaction. Throws on failure to commit.
function commitTransaction(conn, lsid, txnNumber) {
    assert(configuredForTxnOverride());
    assert.gte(txnNumber, 0);

    logMsgFull('commitTransaction',
               `Committing transaction ${txnNumber} on session ${tojsononeline(lsid)}`);

    // Running the command on conn will reenter from the top of `runCommandOverride`, retrying
    // as needed.
    assert.commandWorked(conn.adminCommand({
        commitTransaction: 1,
        autocommit: false,
        lsid: lsid,
        txnNumber: txnNumber,
    }));

    // We've successfully committed the transaction, so we can forget the ops we've successfully
    // run.
    clearOpsList();
}

function abortTransaction(conn, lsid, txnNumber) {
    assert(configuredForTxnOverride());
    assert.gte(txnNumber, 0);

    logMsgFull('abortTransaction',
               `Aborting transaction ${txnNumber} on session ${tojsononeline(lsid)}`);

    // Running the command on conn will reenter from the top of `runCommandOverride`, retrying
    // as needed.
    const res = conn.adminCommand({
        abortTransaction: 1,
        autocommit: false,
        lsid: lsid,
        txnNumber: txnNumber,
    });

    // Transient transaction errors mean the transaction has aborted, so consider it a success.
    if (TransactionsUtil.isTransientTransactionError(res)) {
        return;
    }
    assert.commandWorked(res);
}

function startNewTransaction(conn, cmdObj) {
    // Bump the txnNumber and reset the stmtId.
    txnOptions.txnNumber = new NumberLong(txnOptions.txnNumber + 1);
    txnOptions.stmtId = new NumberInt(1);

    // Used to communicate the txnNumber to unittests.
    TestData.currentTxnOverrideTxnNumber = txnOptions.txnNumber;

    cmdObj.startTransaction = true;
    return txnOptions.txnNumber;
}

function calculateStmtIdInc(cmdName, cmdObj) {
    // Reserve the statement ids for batch writes.
    try {
        switch (cmdName) {
            case "insert":
                return cmdObj.documents.length;
            case "update":
                return cmdObj.updates.length;
            case "delete":
                return cmdObj.deletes.length;
            case "bulkWrite":
                return cmdObj.ops.length;
            default:
                return 1;
        }
    } catch (e) {
        // Malformed command objects can cause errors to be thrown.
        return 1;
    }
}

function continueTransaction(conn, dbName, cmdName, cmdObj) {
    cmdObj.txnNumber = txnOptions.txnNumber;
    cmdObj.stmtId = txnOptions.stmtId;
    cmdObj.autocommit = false;

    // Bump the stmtId for the next statement. We do this after so that the stmtIds start at 1.
    txnOptions.stmtId = new NumberInt(txnOptions.stmtId + calculateStmtIdInc(cmdName, cmdObj));

    // This function expects to get a command without any read or write concern properties.
    assert(!cmdObj.hasOwnProperty('readConcern'), cmdObj);
    assert(!cmdObj.hasOwnProperty('writeConcern'), cmdObj);

    // If this is the first time we are running this command, push it to the ops array.
    if (!isNested() && !inCommandNetworkErrorRetry) {
        // Make a copy so the command does not get changed by the test.
        const objCopy = TransactionsUtil.deepCopyObject({}, cmdObj);

        // Empty transaction state that needs to be refreshed. The stmtId and startTransaction
        // fields shouldn't need to be refreshed.
        delete objCopy.txnNumber;
        delete objCopy.$clusterTime;

        ops.push({
            dbName: dbName,
            cmdObj: objCopy,
        });
    }
}

// Returns true iff a command is a "getMore" on a cursor that is in the `nonTxnAggCursorSet`
// dictionary of cursors that were created outside of any transaction.
function isCommandNonTxnGetMore(cmdName, cmdObj) {
    return cmdName === "getMore" && nonTxnAggCursorSet[cmdObj.getMore];
}

function isNamespaceSystemDotProfile(cmdObj) {
    // No operations on system.profile are permitted inside transactions (see SERVER-46900).
    for (let val of Object.values(cmdObj)) {
        if (typeof val === 'string' && val.endsWith('system.profile')) {
            return true;
        }
    }
    return false;
}

function setupTransactionCommand(conn, dbName, cmdName, cmdObj, lsid) {
    // We want to overwrite whatever read and write concern is already set.
    delete cmdObj.readConcern;
    delete cmdObj.writeConcern;

    // If sessions are explicitly disabled for this command, we skip overriding it to
    // use transactions.
    const driverSession = conn.getDB(dbName).getSession();
    const commandSupportsTransaction = TransactionsUtil.commandSupportsTxn(dbName, cmdName, cmdObj);
    const isSystemDotProfile = isNamespaceSystemDotProfile(cmdObj);

    if (commandSupportsTransaction && !isSystemDotProfile &&
        driverSession.getSessionId() !== null && !isCommandNonTxnGetMore(cmdName, cmdObj)) {
        if (isNested()) {
            // Nested commands should never start a new transaction.
        } else if (ops.length === 0) {
            // We should never end a transaction on a getMore.
            assert.neq(cmdName, "getMore", cmdObj);
            startNewTransaction(conn, cmdObj);
        } else if (cmdName === "getMore") {
            // If the command is a getMore, we cannot consider ending the transaction.
        } else if (ops.length >= maxOpsInTransaction) {
            logMsgFull('setupTransactionCommand',
                       `Committing transaction ${txnOptions.txnNumber} on session` +
                           ` ${tojsononeline(lsid)} because we have hit max ops length`);
            commitTransaction(conn, lsid, txnOptions.txnNumber);
            startNewTransaction(conn, cmdObj);
        }
        continueTransaction(conn, dbName, cmdName, cmdObj);

    } else {
        if (ops.length > 0 && !isNested() && !isSystemDotProfile) {
            // Operations on system.profile must be allowed to execute in parallel with open
            // transactions, so operations on system.profile should not commit the current open
            // transaction.
            logMsgFull('setupTransactionCommand',
                       `Committing transaction ${txnOptions.txnNumber} on session` +
                           ` ${tojsononeline(lsid)} to run a command that does not support` +
                           ` transactions: ${cmdName}`);
            commitTransaction(conn, lsid, txnOptions.txnNumber);
        }
    }
    appendReadAndWriteConcern(conn, dbName, cmdName, cmdObj);
}

// Retries the entire transaction without committing it. Returns immediately on an error with
// the response from the failed command. This may recursively retry the entire transaction in
// which case parent retries are completed early.
function retryEntireTransaction(conn, lsid) {
    // Re-run every command in the ops array.
    assert.gt(ops.length, 0);

    // Keep track of what txnNumber this retry is attempting.
    const retriedTxnNumber = startNewTransaction(conn, {"ignored object": 1});

    logMsgFull('Retrying entire transaction',
               `txnNumber: ${retriedTxnNumber}, lsid: ${tojsononeline(lsid)}`);
    let res;
    for (let op of ops) {
        logMsgFull('Retrying op',
                   `txnNumber: ${retriedTxnNumber}, lsid: ${tojsononeline(lsid)},` +
                       ` db: ${op.dbName}, op: ${tojsononeline(op.cmdObj)}`);
        // Running the command on conn will reenter from the top of `runCommandOverride`,
        // individual statement retries will be suppressed by tracking nesting level.
        res = conn.getDB(op.dbName).runCommand(op.cmdObj);

        if (hasError(res) || hasWriteConcernError(res)) {
            return res;
        }
        // Sanity check that we checked for an error correctly.
        assert.commandWorked(res);

        // If we recursively retried the entire transaction, we do not want to continue this
        // retry. We just pass up the response from the retry that completed.
        if (txnOptions.txnNumber !== retriedTxnNumber) {
            return res;
        }
    }

    // We do not commit the transaction and let it continue in the next operation.
    return res;
}

// Creates the given collection, retrying if needed. Throws on failure.
function createCollectionExplicitly(conn, dbName, collName, lsid) {
    logMsgFull(
        'create',
        `Explicitly creating collection ${dbName}.${collName} and then retrying transaction`);

    // Always majority commit the create because this is not expected to roll back once
    // successful.
    const createCmdObj = {
        create: collName,
        lsid: lsid,
        writeConcern: {w: 'majority'},
    };

    // Running the command on conn will reenter from the top of `runCommandOverride`, retrying
    // as needed. If an error returned by `create` were tolerable, it would already have been
    // retried by the time it surfaced here.
    assert.commandWorked(conn.getDB(dbName).runCommand(createCmdObj));
}

// Processes the response to the command if we are configured for txn override. Performs retries
// if necessary for implicit collection creation or transient transaction errors.
// Returns the last command response received by a command or retry.
function retryWithTxnOverride(res, conn, dbName, cmdName, cmdObj, lsid, logError) {
    assert(configuredForTxnOverride());

    const failedOnCRUDStatement =
        hasError(res) && !isCommitOrAbort(cmdName) && isCmdInTransaction(cmdObj);
    if (failedOnCRUDStatement) {
        assert.gt(ops.length, 0);
        abortTransaction(conn, lsid, txnOptions.txnNumber);

        // Transaction statements cannot be retried, but retryable codes are expected to succeed
        // on full transaction retry.
        if (configuredForNetworkRetry() && RetryableWritesUtil.isRetryableCode(res.code)) {
            logError("Retrying on retryable error for transaction statement");
            return retryEntireTransaction(conn, lsid);
        }
    }

    // Transient transaction errors should retry the entire transaction. A
    // TransientTransactionError on "abortTransaction" is considered a success.
    if (TransactionsUtil.isTransientTransactionError(res) && cmdName !== "abortTransaction") {
        logError("Retrying on TransientTransactionError response");
        res = retryEntireTransaction(conn, lsid);

        // If we got a TransientTransactionError on 'commitTransaction' retrying the transaction
        // will not retry it, so we retry it here.
        if (!hasError(res) && cmdName === "commitTransaction") {
            commitTransaction(conn, lsid, txnOptions.txnNumber);
        }
        return res;
    }

    return res;
}

// Returns true if any error code in a response's "raw" field is retryable.
function rawResponseHasRetryableError(rawRes, cmdName, startTime, logError) {
    for (let shard in rawRes) {
        const shardRes = rawRes[shard];

        const logShardError = (msg) => {
            const msgWithShardPrefix = `Processing raw response from shard: ${shard} :: ${msg}`;
            logError(msgWithShardPrefix);
        };

        // Don't override the responses from each shard because only the top-level code in a
        // response is used to determine if a command succeeded or not.
        const networkRetryShardRes = shouldRetryWithNetworkErrorOverride(
            shardRes, cmdName, startTime, logShardError, false /* shouldOverrideAcceptableError */);
        if (networkRetryShardRes === kContinue) {
            return true;
        }
    }
    return false;
}

const kContinue = Object.create(null);

// Processes the command response if we are configured for network error retries. Returns the
// provided response if we should not retry in this override. Returns kContinue if we should
// retry the current command without subtracting from our retry allocation. By default sets ok=1
// for failures with acceptable error codes, unless shouldOverrideAcceptableError is false.
function shouldRetryWithNetworkErrorOverride(
    res, cmdName, startTime, logError, shouldOverrideAcceptableError = true) {
    assert(configuredForNetworkRetry());

    if (RetryableWritesUtil.isFailedToSatisfyPrimaryReadPreferenceError(res) &&
        Date.now() - startTime < 5 * 60 * 1000) {
        // ReplicaSetMonitor::getHostOrRefresh() waits up to 15 seconds to find the
        // primary of the replica set. It is possible for the step up attempt of another
        // node in the replica set to take longer than 15 seconds so we allow retrying
        // for up to 5 minutes.
        logError("Failed to find primary when attempting to run command," +
                 " will retry for another 15 seconds");
        return kContinue;
    }

    if (RetryableWritesUtil.isRetryableWriteCmdName(cmdName)) {
        if ((cmdName === "findandmodify" || cmdName === "findAndModify") &&
            isRetryableExecutorCodeAndMessage(res.code, res.errmsg)) {
            // findAndModify can fail during the find stage and return an executor error.
            logError("Retrying because of executor interruption");
            return kContinue;
        }

        // Don't interfere with retryable writes.
        return res;
    }

    // commitTransaction should be retried on any write concern error.
    if (cmdName === "commitTransaction" && hasWriteConcernError(res)) {
        logError("Retrying write concern error response for commitTransaction");
        return kContinue;
    }

    if (cmdName === "explain") {
        // If an explain is interrupted by a stepdown, and it returns before its connection is
        // closed, it will return incomplete results. To prevent failing the test, force retries
        // of interrupted explains.
        if (res.hasOwnProperty("executionStats") && !res.executionStats.executionSuccess &&
            (RetryableWritesUtil.isRetryableCode(res.executionStats.errorCode) ||
             isRetryableExecutorCodeAndMessage(res.executionStats.errorCode,
                                               res.executionStats.errorMessage))) {
            logError("Forcing retry of interrupted explain");
            return kContinue;
        }

        // An explain command can fail if its child command cannot be run on the current server.
        // This can be hit if a primary only or not explicitly slaveOk command is accepted by a
        // primary node that then steps down and returns before having its connection closed.
        if (!res.ok && res.errmsg.indexOf("child command cannot run on this node") >= 0) {
            logError("Forcing retry of explain likely interrupted by transition to secondary");
            return kContinue;
        }
    }

    if (!res.ok) {
        if (RetryableWritesUtil.isRetryableCode(res.code)) {
            // Don't decrement retries, because the command returned before the connection was
            // closed, so a subsequent attempt will receive a network error (or NotWritablePrimary
            // error) and need to retry.
            logError("Retrying failed response with retryable code");
            return kContinue;
        }

        if (isRetryableExecutorCodeAndMessage(res.code, res.errmsg)) {
            logError("Retrying because of executor interruption");
            return kContinue;
        }

        // Some sharding commands return raw responses from all contacted shards and there won't
        // be a top level code if shards returned more than one error code, in which case retry
        // if any error is retryable.
        if (res.hasOwnProperty("raw") && !res.hasOwnProperty("code") &&
            rawResponseHasRetryableError(res.raw, cmdName, startTime, logError)) {
            logError("Retrying because of retryable code in raw response");
            return kContinue;
        }

        // Check for the retryable error codes from an interrupted shardCollection.
        if (cmdName === "shardCollection" && isRetryableShardCollectionResponse(res)) {
            logError("Retrying interrupted shardCollection");
            return kContinue;
        }

        // Check for the retryable error codes from an interrupted moveChunk.
        if (cmdName === "moveChunk" && isRetryableMoveChunkResponse(res)) {
            logError("Retrying interrupted moveChunk");
            return kContinue;
        }

        // In a sharded cluster, drop may bury the original error code in the error message if
        // interrupted.
        if (cmdName === "drop" && RetryableWritesUtil.errmsgContainsRetryableCodeName(res.errmsg)) {
            logError("Retrying interrupted drop");
            return kContinue;
        }

        if (!shouldOverrideAcceptableError || !isAcceptableRetryFailedResponse(cmdName, res)) {
            // Pass up unretryable errors.
            return res;
        }

        // Swallow safe errors that may come from a retry since the command may have completed
        // before the connection was closed.
        logError("Overriding safe failed response for");
        res.ok = 1;

        // Fall through to retry on write concern errors if needed.
    }

    // Do not retry on a write concern error at this point if there is an actual error.
    // TransientTransactionErrors would already have been retried at an earlier point.
    if (hasWriteConcernError(res) && !hasError(res)) {
        if (RetryableWritesUtil.isRetryableCode(res.writeConcernError.code)) {
            logError("Retrying write concern error response with retryable code");
            return kContinue;
        }
    }

    return res;
}

function shouldRetryForBackgroundReconfigOverride(res, cmdName, logError) {
    assert(configuredForBackgroundReconfigs());
    // Background reconfigs can interfere with read commands if they are using readConcern: majority
    // and readPreference: primary. If we're running a read command and it fails with
    // ReadConcernMajorityNotAvailableYet, retry because it should eventually succeed.
    if (isRetryableReadCmdName(cmdName) && isRetryableReadCode(res.code)) {
        logError("Retrying read command after 100ms because of background reconfigs");
        sleep(100);
        return kContinue;
    }
    return res;
}

// Processes exceptions if configured for txn override. Retries the entire transaction on
// transient transaction errors or network errors if configured for network errors as well.
// If a retry fails, returns the response, or returns null for further exception processing.
function retryWithTxnOverrideException(e, conn, cmdName, cmdObj, lsid, logError) {
    assert(configuredForTxnOverride());

    if (TransactionsUtil.isTransientTransactionError(e) && cmdName !== "abortTransaction") {
        logError("Retrying on TransientTransactionError exception for command");
        const res = retryEntireTransaction(conn, lsid);

        // If we got a TransientTransactionError on 'commitTransaction' retrying the transaction
        // will not retry it, so we retry it here.
        if (!hasError(res) && cmdName === "commitTransaction") {
            commitTransaction(conn, lsid, txnOptions.txnNumber);
        }
        return res;
    }

    if (configuredForNetworkRetry() && isNetworkError(e) &&
        !canRetryNetworkErrorForCommand(cmdName, cmdObj)) {
        logError("Retrying on network exception for transaction statement");
        return retryEntireTransaction(conn, lsid);
    }
    return null;
}

// Processes exceptions if configured for network error retry. Returns whether to subtract one
// from the number of command retries this override counts. Throws if we should not retry.
function shouldRetryWithNetworkExceptionOverride(
    e, cmdName, cmdObj, startTime, numNetworkErrorRetries, logError) {
    assert(configuredForNetworkRetry());

    if (numNetworkErrorRetries === 0) {
        logError("No retries, throwing");
        throw e;
    } else if (RetryableWritesUtil.isFailedToSatisfyPrimaryReadPreferenceError(e) &&
               Date.now() - startTime < 5 * 60 * 1000) {
        // ReplicaSetMonitor::getHostOrRefresh() waits up to 15 seconds to find the
        // primary of the replica set. It is possible for the step up attempt of another
        // node in the replica set to take longer than 15 seconds so we allow retrying
        // for up to 5 minutes.
        logError("Failed to find primary when attempting to run command," +
                 " will retry for another 15 seconds");
        return false;
    } else if ((e.message.indexOf("writeConcernError") >= 0) && isRetryableError(e)) {
        logError("Retrying write concern error exception with retryable code");
        return false;
    } else if (!isNetworkError(e)) {
        logError("Not a network error, throwing");
        throw e;
    } else if (RetryableWritesUtil.isRetryableWriteCmdName(cmdName)) {
        if (_ServerSession.canRetryWrites(cmdObj)) {
            // If the command is retryable, assume the command has already gone through
            // or will go through the retry logic in SessionAwareClient, so propagate
            // the error.
            logError("Letting retryable writes code retry, throwing");
            throw e;
        }
    }

    logError("Retrying on ordinary network error, subtracting from retry count");
    return true;
}

const kMaxNumRetries = 3;

// This function is the heart of the override with the main error retry loop.
function runCommandOverrideBody(conn, dbName, cmdName, cmdObj, lsid, clientFunction, makeFuncArgs) {
    const startTime = Date.now();

    const isTxnStatement = isCmdInTransaction(cmdObj);

    if (configuredForNetworkRetry() && !isNested() && !isTxnStatement) {
        // If this is a top level command, make sure that the command supports network error
        // retries. Don't validate transaction statements because their encompassing transaction
        // can be retried at a higher level, even if each statement isn't retryable on its own.
        validateCmdNetworkErrorCompatibility(cmdName, cmdObj);
    }

    if (configuredForTxnOverride()) {
        setupTransactionCommand(conn, dbName, cmdName, cmdObj, lsid);
    }

    const canRetryNetworkError = canRetryNetworkErrorForCommand(cmdName, cmdObj);
    const canRetryReadError = canRetryReadErrorDuringBackgroundReconfig(cmdName);
    let numNetworkErrorRetries = canRetryNetworkError ? kMaxNumRetries : 0;
    do {
        try {
            // Actually run the provided command.
            let res = clientFunction.apply(conn, makeFuncArgs(cmdObj));
            if (configuredForTxnOverride()) {
                logMsgFull("Override got response",
                           `res: ${tojsononeline(res)}, cmd: ${tojsononeline(cmdObj)}`);

                if (!hasError(res) &&
                    TransactionsUtil.commandIsNonTxnAggregation(cmdName, cmdObj)) {
                    nonTxnAggCursorSet[res.cursor.id] = true;
                }
            }

            const logError = (msg) => logErrorFull(msg, cmdName, cmdObj, res);

            if (configuredForTxnOverride()) {
                res = retryWithTxnOverride(res, conn, dbName, cmdName, cmdObj, lsid, logError);
            }

            if (canRetryNetworkError) {
                const networkRetryRes =
                    shouldRetryWithNetworkErrorOverride(res, cmdName, startTime, logError);
                if (networkRetryRes === kContinue) {
                    continue;
                } else {
                    res = networkRetryRes;
                }
            }

            if (canRetryReadError) {
                const readRetryRes =
                    shouldRetryForBackgroundReconfigOverride(res, cmdName, logError);
                if (readRetryRes === kContinue) {
                    continue;
                } else {
                    res = readRetryRes;
                }
            }

            return res;

        } catch (e) {
            const logError = (msg) => logErrorFull(msg, cmdName, cmdObj, e);

            if (configuredForTxnOverride()) {
                const txnRetryOnException =
                    retryWithTxnOverrideException(e, conn, cmdName, cmdObj, lsid, logError);
                if (txnRetryOnException) {
                    return txnRetryOnException;
                }
            }

            if (canRetryNetworkError) {
                const decrementRetryCount = shouldRetryWithNetworkExceptionOverride(
                    e, cmdName, cmdObj, startTime, numNetworkErrorRetries, logError);
                if (decrementRetryCount) {
                    --numNetworkErrorRetries;
                    logMsgFull("Decrementing command network error retry count",
                               `New count: ${numNetworkErrorRetries}`);
                }

                logErrorFull("Retrying on network error for command", cmdName, cmdObj, e);
                inCommandNetworkErrorRetry = true;
                continue;
            }

            throw e;
        }
    } while (numNetworkErrorRetries >= 0);
    throw new Error("MONGO UNREACHABLE");
}

// Top level runCommand override function.
function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    currentCommandID.push(newestCommandID++);
    nestingLevel++;

    const lsid = cmdObj.lsid;
    try {
        const res = runCommandOverrideBody(
            conn, dbName, cmdName, cmdObj, lsid, clientFunction, makeFuncArgs);

        // Many tests run queries that are expected to fail. In this case, when we wrap CRUD ops
        // in transactions, the transaction including the failed query will not be able to
        // commit. This override expects transactions to be able to commit. Rather than
        // denylisting all tests containing queries that are expected to fail, we clear the ops
        // list when we return an error to the test so we do not retry the failed query.
        if (configuredForTxnOverride() && !isNested() && hasError(res) && (ops.length > 0)) {
            logMsgFull("Clearing ops on failed command",
                       `res: ${tojsononeline(res)}, cmd: ${tojsononeline(cmdObj)}`);
            clearOpsList();
            abortTransaction(conn, lsid, txnOptions.txnNumber);
        }

        return res;
    } finally {
        // Reset recursion and retry state tracking.
        nestingLevel--;
        currentCommandID.pop();
        inCommandNetworkErrorRetry = false;
    }
}

if (configuredForNetworkRetry()) {
    const connectOriginal = connect;

    globalThis.connect = function(url, user, pass) {
        let retVal;

        let connectionAttempts = 0;
        assert.soon(
            () => {
                try {
                    connectionAttempts += 1;
                    retVal = connectOriginal.apply(this, arguments);
                    return true;
                } catch (e) {
                    print(kLogPrefix + " Retrying connection to: " + url +
                          ", attempts: " + connectionAttempts + ", failed with: " + tojson(e));
                }
            },
            "Failed connecting to url: " + tojson(url),
            undefined,  // Default timeout.
            2000);      // 2 second interval.

        return retVal;
    };

    Mongo.prototype.logout = function() {
        throw new Error(
            "logout() isn't resilient to network errors. Please add requires_non_retryable_commands to your test");
    };

    globalThis.startParallelShell = function() {
        throw new Error("Cowardly refusing to run test with network retries enabled when it uses " +
                        "startParallelShell()");
    };
}

if (configuredForTxnOverride()) {
    globalThis.startParallelShell = function() {
        throw new Error(
            "Cowardly refusing to run test with transaction override enabled when it uses " +
            "startParallelShell()");
    };
}

OverrideHelpers.overrideRunCommand(runCommandOverride);
