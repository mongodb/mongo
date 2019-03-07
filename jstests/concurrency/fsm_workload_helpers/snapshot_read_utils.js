/**
 * Helpers for doing a snapshot read in concurrency suites. Specifically, the read is a find that
 * spans a getmore.
 */
load('jstests/concurrency/fsm_workload_helpers/cleanup_txns.js');
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
 * Performs a snapshot find.
 */
function doSnapshotFind(sortByAscending, collName, data, findErrorCodes) {
    // Reset txnNumber and stmtId for this transaction.
    const abortErrorCodes = [
        ErrorCodes.NoSuchTransaction,
        ErrorCodes.TransactionCommitted,
        ErrorCodes.TransactionTooOld,
        ErrorCodes.Interrupted
    ];
    abortTransaction(data.sessionDb, data.txnNumber, abortErrorCodes);
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
    assert.commandWorkedOrFailedWithCode(res, findErrorCodes, () => `cmd: ${tojson(findCmd)}`);
    const cursor = parseCursor(res);

    if (!cursor) {
        abortTransaction(
            data.sessionDb, data.txnNumber, [ErrorCodes.NoSuchTransaction, ErrorCodes.Interrupted]);
        data.cursorId = NumberLong(0);
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
    if (bsonWoCompare({_: data.cursorId}, {_: NumberLong(0)}) === 0) {
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
    assert.commandWorkedOrFailedWithCode(
        res, getMoreErrorCodes, () => `cmd: ${tojson(getMoreCmd)}`);

    const commitCmd = {
        commitTransaction: 1,
        txnNumber: NumberLong(data.txnNumber),
        autocommit: false
    };
    res = data.sessionDb.adminCommand(commitCmd);
    assert.commandWorkedOrFailedWithCode(
        res, commitTransactionErrorCodes, () => `cmd: ${tojson(commitCmd)}`);
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
