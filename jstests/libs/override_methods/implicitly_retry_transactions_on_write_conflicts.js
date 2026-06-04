/**
 * Overrides runCommand so that multi-document transactions that encounter a WriteConflict
 * (TransientTransactionError) are automatically retried from the beginning.
 *
 * On a TransientTransactionError the server has already aborted the transaction, so we replay every
 * statement issued since startTransaction. A replica set requires a strictly higher txnNumber to
 * restart a transaction on a session. The driver session in the shell is unaware that we restarted
 * under a higher number, so it keeps issuing subsequent commands (e.g. prepareTransaction,
 * commitTransaction) with its original client-side txnNumber. We therefore track a per-session
 * offset and translate every command's txnNumber from the client value to the server value.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Bound on transaction restarts to avoid looping forever if a transient error is somehow
// persistent.
const kMaxTransactionRetries = 10;

// Serialized lsid -> {offset, buffer}, where 'offset' is added to the client-side txnNumber to
// derive the server-side txnNumber, and 'buffer' holds the statements of the in-progress
// transaction so the whole transaction can be replayed on a restart.
const sessionState = new Map();

function getSessionState(sessionId) {
    let state = sessionState.get(sessionId);
    if (!state) {
        state = {offset: 0, buffer: []};
        sessionState.set(sessionId, state);
    }
    return state;
}

function isInTransaction(cmdObj) {
    return cmdObj.hasOwnProperty("txnNumber") && cmdObj.autocommit === false;
}

function isTransientTransactionError(res) {
    return res.ok === 0 && res.hasOwnProperty("errorLabels") && res.errorLabels.includes("TransientTransactionError");
}

// Replays every buffered statement of the current transaction under the current (already-bumped)
// offset, marking the first statement with startTransaction. Returns the result of the last
// statement, or the first statement that fails (transiently or otherwise) so the caller can decide
// whether to restart again.
function replayTransactionBody(state, runTranslated) {
    let res = {ok: 1};
    for (let i = 0; i < state.buffer.length; i++) {
        const entry = state.buffer[i];
        const replayCmd = Object.assign({}, entry.cmdObj);
        if (i === 0) {
            replayCmd.startTransaction = true;
        } else {
            delete replayCmd.startTransaction;
        }
        res = runTranslated(replayCmd, entry.dbName);
        if (!res.ok) {
            return res;
        }
    }
    return res;
}

function runCommandWithTxnRetry(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    // Only commands carrying a session txnNumber are affected by transaction restarts.
    if (
        typeof commandObj !== "object" ||
        commandObj === null ||
        !commandObj.hasOwnProperty("lsid") ||
        !commandObj.hasOwnProperty("txnNumber")
    ) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    const sessionId = tojson(commandObj.lsid);
    const state = getSessionState(sessionId);

    // Translate the driver's client-side txnNumber to the (possibly higher) server-side txnNumber
    // for this session, then run the command.
    function runTranslated(cmdObj, db) {
        if (state.offset === 0) {
            return func.apply(conn, makeFuncArgs(cmdObj, db));
        }
        const translated = Object.assign({}, cmdObj);
        translated.txnNumber = NumberLong(cmdObj.txnNumber + state.offset);
        return func.apply(conn, makeFuncArgs(translated, db));
    }

    // Retryable writes and other non-transaction commands only need txnNumber translation so that
    // they stay strictly increasing after a restart bumped the session's offset.
    if (!isInTransaction(commandObj)) {
        return runTranslated(commandObj, dbName);
    }

    // A new transaction resets the replay buffer.
    if (commandObj.startTransaction === true) {
        state.buffer = [];
    }

    // abortTransaction or prepared commitTransaction (which carries commitTimestamp) end the
    // transaction; just drop the buffer, translate, and run.
    if (
        commandName === "abortTransaction" ||
        (commandName === "commitTransaction" && commandObj.hasOwnProperty("commitTimestamp"))
    ) {
        state.buffer = [];
        return runTranslated(commandObj, dbName);
    }

    // Buffer this statement so the whole transaction can be replayed under a higher txnNumber if it
    // aborts transiently. An unprepared commit can itself hit a write conflict; since it is now the
    // last buffered statement, replaying the buffer restarts the transaction and re-issues the
    // commit.
    state.buffer.push({dbName, cmdObj: Object.assign({}, commandObj)});

    let res = runTranslated(commandObj, dbName);

    let attempt = 0;
    while (isTransientTransactionError(res) && attempt < kMaxTransactionRetries) {
        attempt++;
        // The server aborted the transaction. Restart it under a strictly higher txnNumber and
        // replay every buffered statement. The loop exits on success or on a non-transient failure
        // (which is returned to the caller).
        state.offset++;
        jsTest.log.info(
            "implicitly_retry_transactions_on_write_conflicts: retrying transaction on TransientTransactionError",
            {sessionId, failedCommand: commandName, attempt},
        );
        res = replayTransactionBody(state, runTranslated);
    }

    // An unprepared commit ends the transaction; drop the buffer once the commit attempt finishes.
    if (commandName === "commitTransaction") {
        state.buffer = [];
    }

    return res;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_retry_transactions_on_write_conflicts.js",
);
OverrideHelpers.overrideRunCommand(runCommandWithTxnRetry);
