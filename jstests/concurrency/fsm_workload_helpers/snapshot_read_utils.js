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
        const abortCmd = {
            abortTransaction: 1,
            txnNumber: NumberLong(data.txnNumber),
            autocommit: false
        };
        res = data.sessionDb.adminCommand(abortCmd);
        const abortErrorCodes = [
            ErrorCodes.NoSuchTransaction,
            ErrorCodes.TransactionCommitted,
            ErrorCodes.TransactionTooOld,
            ErrorCodes.Interrupted
        ];
        assertWorkedOrFailed(abortCmd, res, abortErrorCodes);
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
function insertSessionDoc(db, collName, data, session) {
    const sessionDoc = {"_id": "sessionDoc" + data.tid, "id": session.getSessionId().id};
    const res = db[collName].insert(sessionDoc);
    assert.writeOK(res);
    assert.eq(1, res.nInserted);
}

/**
 * This function can be used in conjunction with insertSessionDoc to kill any active sessions on
 * teardown.
 */
function killSessionsFromDocs(db, collName) {
    const sessionDocCursor = db[collName].find({"_id": {$regex: "sessionDoc*"}});
    assert(sessionDocCursor.hasNext());
    while (sessionDocCursor.hasNext()) {
        const sessionDoc = sessionDocCursor.next();
        assert.commandWorked(db.runCommand({killSessions: [{id: sessionDoc.id}]}));
    }
}
