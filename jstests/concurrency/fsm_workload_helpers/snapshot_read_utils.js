/**
 * Helpers for doing a snapshot read in concurrency suites. Specifically, the read is a find that
 * spans a getmore.
 */

/**
 * Parses a cursor from cmdResult, if possible.
 */
function parseCursor(cmdResult) {
    if (cmdResult.hasOwnProperty("cursor")) {
        assert(cmdResult.cursor.hasOwnProperty("id"));
        return cmdResult.cursor;
    }
    return null;
}

/**
 * Asserts cmd has either failed with a code in a specified set of codes or has succeeded.
 */
function assertWorkedOrFailed(cmd, cmdResult, errorCodeSet) {
    if (!cmdResult.ok) {
        assert.commandFailedWithCode(cmdResult,
                                     errorCodeSet,
                                     "expected command to fail with one of " + errorCodeSet +
                                         ", cmd: " + tojson(cmd) + ", result: " +
                                         tojson(cmdResult));
    } else {
        assert.commandWorked(cmdResult);
    }
}

/**
 * Performs a snapshot find.
 */
function doSnapshotFind(sortByAscending, collName, data, findErrorCodes) {
    // Reset txnNumber and stmtId for this transaction.
    data.txnNumber++;
    data.stmtId = 0;

    const sortOrder = sortByAscending ? {_id: 1} : {_id: -1};
    const findCmd = {
        find: collName,
        sort: sortOrder,
        batchSize: 0,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(data.txnNumber),
        stmtId: NumberInt(data.stmtId++),
        startTransaction: true,
        autocommit: false
    };

    // Establish a snapshot batchSize:0 cursor.
    let res = data.sessionDb.runCommand(findCmd);
    assertWorkedOrFailed(findCmd, res, findErrorCodes);
    const cursor = parseCursor(res);

    if (!cursor) {
        abortTransaction(data.sessionDb, data.txnNumber, [ErrorCodes.NoSuchTransaction]);
        data.cursorId = 0;
    } else {
        assert(cursor.hasOwnProperty("firstBatch"), tojson(res));
        assert.eq(0, cursor.firstBatch.length, tojson(res));
        assert.neq(cursor.id, 0);

        // Store the cursor Id in the data object.
        data.cursorId = cursor.id;
    }
}

/**
 * Performs a snapshot getmore. This function is to be used in conjunction with doSnapshotFind.
 */
function doSnapshotGetMore(collName, data, getMoreErrorCodes, commitTransactionErrorCodes) {
    // doSnapshotGetMore may be called even if doSnapshotFind fails to obtain a cursor.
    if (!data.cursorId) {
        return;
    }
    const getMoreCmd = {
        getMore: data.cursorId,
        collection: collName,
        batchSize: data.batchSize,
        txnNumber: NumberLong(data.txnNumber),
        stmtId: NumberInt(data.stmtId++),
        autocommit: false
    };
    let res = data.sessionDb.runCommand(getMoreCmd);
    assertWorkedOrFailed(getMoreCmd, res, getMoreErrorCodes);

    const commitCmd = {
        commitTransaction: 1,
        txnNumber: NumberLong(data.txnNumber),
        stmtId: NumberInt(data.stmtId++),
        autocommit: false
    };
    res = data.sessionDb.adminCommand(commitCmd);
    assertWorkedOrFailed(commitCmd, res, commitTransactionErrorCodes);
}

/**
 * This function can be used to share session data across threads.
 */
function insertSessionDoc(db, collName, tid, sessionId) {
    const sessionDoc = {"_id": "sessionDoc" + tid, "id": sessionId};
    const res = db[collName].insert(sessionDoc);
    assert.writeOK(res);
    assert.eq(1, res.nInserted);
}

/**
 * This function can be used in conjunction with insertSessionDoc to kill any active sessions on
 * teardown or iteration completion.
 */
function killSessionsFromDocs(db, collName, tid) {
    // Cleanup up all sessions, unless 'tid' is supplied.
    let docs = {$regex: /^sessionDoc/};
    if (tid !== undefined) {
        docs = "sessionDoc" + tid;
    }
    let sessionIds = db[collName].find({"_id": docs}, {_id: 0, id: 1}).toArray();
    assert.commandWorked(db.runCommand({killSessions: sessionIds}));
}

/**
 * Abort the transaction on the session and return result.
 */
function abortTransaction(db, txnNumber, errorCodes) {
    abortCmd = {abortTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false};
    res = db.adminCommand(abortCmd);
    assertWorkedOrFailed(abortCmd, res, errorCodes);
    return res;
}

/**
 * This function operates on the last iteration of each thread to abort any active transactions.
 */
var {cleanupOnLastIteration} = (function() {
    function cleanupOnLastIteration(data, func) {
        const abortErrorCodes = [
            ErrorCodes.NoSuchTransaction,
            ErrorCodes.TransactionCommitted,
            ErrorCodes.TransactionTooOld
        ];
        let lastIteration = ++data.iteration >= data.iterations;
        try {
            func();
        } catch (e) {
            lastIteration = true;
            throw e;
        } finally {
            if (lastIteration) {
                // Abort the latest transactions for this session as some may have been skipped due
                // to incrementing data.txnNumber. Go in increasing order, so as to avoid bumping
                // the txnNumber on the server past that of an in-progress transaction. See
                // SERVER-36847.
                for (let i = 0; i <= data.txnNumber; i++) {
                    let res = abortTransaction(data.sessionDb, i, abortErrorCodes);
                    if (res.ok === 1) {
                        break;
                    }
                }
            }
        }
    }

    return {cleanupOnLastIteration};
})();
