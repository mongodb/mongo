// Tests that the asychronous thread that aborts expired transactions will not get stuck behind a
// drop command blocked on two transactions. A drop cmd requires a database exclusive lock, which
// will block behind transactions holding database intent locks. Aborting a transaction must not
// require taking further locks that would queue up behind the drop cmd's database exclusive lock
// request.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession, setParameter.
//   not_allowed_with_signed_security_token,
//   uses_transactions
// ]

const dbName = "test";
const collName = "abort_transaction_thread_does_not_block_on_locks";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];
const sessionOptions = {
    causalConsistency: false,
};

let dropRes = testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
if (!dropRes.ok) {
    assert.commandFailedWithCode(dropRes, ErrorCodes.NamespaceNotFound);
}

const bulk = testColl.initializeUnorderedBulkOp();
for (let i = 0; i < 4; ++i) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute({w: "majority"}));

const res = assert.commandWorked(db.adminCommand({getParameter: 1, transactionLifetimeLimitSeconds: 1}));
const originalTransactionLifetimeLimitSeconds = res.transactionLifetimeLimitSeconds;

try {
    let transactionLifeTime = 10;
    jsTest.log("Decrease transactionLifetimeLimitSeconds to " + transactionLifeTime + " seconds.");
    assert.commandWorked(db.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: transactionLifeTime}));

    // Set up two transactions with IX locks and cursors.

    let session1 = db.getMongo().startSession(sessionOptions);
    let sessionDb1 = session1.getDatabase(dbName);
    let sessionColl1 = sessionDb1[collName];

    let session2 = db.getMongo().startSession(sessionOptions);
    let sessionDb2 = session2.getDatabase(dbName);
    let sessionColl2 = sessionDb2[collName];

    let firstTxnNumber = 1;
    let secondTxnNumber = 2;

    jsTest.log("Setting up first transaction with an open cursor and IX lock");
    let cursorRes1 = assert.commandWorked(
        sessionDb1.runCommand({
            find: collName,
            batchSize: 2,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(firstTxnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }),
    );
    assert(cursorRes1.hasOwnProperty("cursor"), tojson(cursorRes1));
    assert.neq(0, cursorRes1.cursor.id, tojson(cursorRes1));

    jsTest.log("Setting up second transaction with an open cursor and IX lock");
    let cursorRes2 = assert.commandWorked(
        sessionDb2.runCommand({
            find: collName,
            batchSize: 2,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(secondTxnNumber),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }),
    );
    assert(cursorRes2.hasOwnProperty("cursor"), tojson(cursorRes2));
    assert.neq(0, cursorRes2.cursor.id, tojson(cursorRes2));

    jsTest.log(
        "Perform a drop. This will block until both transactions finish. The " +
            "transactions should expire in " +
            transactionLifeTime * 1.5 +
            " seconds or less.",
    );
    assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

    // Verify and cleanup.

    jsTest.log("Drop finished. Verifying that the transactions were aborted as expected");
    assert.commandFailedWithCode(
        sessionDb1.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(firstTxnNumber),
            stmtId: NumberInt(2),
            autocommit: false,
        }),
        ErrorCodes.NoSuchTransaction,
    );
    assert.commandFailedWithCode(
        sessionDb2.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(secondTxnNumber),
            stmtId: NumberInt(2),
            autocommit: false,
        }),
        ErrorCodes.NoSuchTransaction,
    );

    session1.endSession();
    session2.endSession();
} finally {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            transactionLifetimeLimitSeconds: originalTransactionLifetimeLimitSeconds,
        }),
    );
}
