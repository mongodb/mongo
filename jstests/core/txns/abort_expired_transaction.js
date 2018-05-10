// Tests that a transaction expires and is then aborted by the server. Uses the server parameter
// 'transactionLifetimeLimitSeconds' to lower the transaction lifetime for quicker transaction
// expiration.
//
// @tags: [uses_transactions]

(function() {
    "use strict";

    const testDBName = "testDB";
    const testCollName = "abort_expired_transaction";
    const ns = testDBName + "." + testCollName;
    const testDB = db.getSiblingDB(testDBName);
    const testColl = testDB[testCollName];
    testColl.drop();

    // Need the original 'transactionLifetimeLimitSeconds' value so that we can reset it back at the
    // end of the test.
    const res = assert.commandWorked(
        db.adminCommand({getParameter: 1, transactionLifetimeLimitSeconds: 1}));
    const originalTransactionLifetimeLimitSeconds = res.transactionLifetimeLimitSeconds;

    try {
        jsTest.log("Decrease transactionLifetimeLimitSeconds from " +
                   originalTransactionLifetimeLimitSeconds + " to 1 second.");
        assert.commandWorked(
            db.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: 1}));

        jsTest.log("Create a collection '" + ns + "' outside of the transaction.");
        assert.writeOK(testColl.insert({foo: "bar"}, {writeConcern: {w: "majority"}}));

        jsTest.log("Set up the session.");
        const sessionOptions = {causalConsistency: false};
        const session = db.getMongo().startSession(sessionOptions);
        const sessionDb = session.getDatabase(testDBName);

        let txnNumber = 0;

        jsTest.log("Insert a document starting a transaction.");
        assert.commandWorked(sessionDb.runCommand({
            insert: testCollName,
            documents: [{_id: "insert-1"}],
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
        }));

        // We can deterministically wait for the transaction to be aborted by waiting for currentOp
        // to cease reporting the inactive transaction: the transaction should disappear from the
        // currentOp results once aborted.
        jsTest.log("Wait for the transaction to expire and be aborted.");
        assert.soon(
            function() {
                const sessionFilter = {
                    active: false,
                    opid: {$exists: false},
                    desc: "inactive transaction",
                    "transaction.parameters.txnNumber": NumberLong(txnNumber),
                    "lsid.id": session.getSessionId().id
                };
                const res = db.getSiblingDB("admin").aggregate(
                    [{$currentOp: {allUsers: true, idleSessions: true}}, {$match: sessionFilter}]);
                return (res.itcount() == 0);

            },
            "currentOp reports that the idle transaction still exists, it has not been " +
                "aborted as expected.");

        jsTest.log(
            "Attempt to do a write in the transaction, which should fail because the transaction " +
            "was aborted");
        assert.commandFailedWithCode(sessionDb.runCommand({
            insert: testCollName,
            documents: [{_id: "insert-2"}],
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
        }),
                                     ErrorCodes.NoSuchTransaction);

        session.endSession();
    } finally {
        // Must ensure that the transactionLifetimeLimitSeconds is reset so that it does not impact
        // other tests in the suite.
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            transactionLifetimeLimitSeconds: originalTransactionLifetimeLimitSeconds
        }));
    }
}());
