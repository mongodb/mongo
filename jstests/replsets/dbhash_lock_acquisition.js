/**
 * Tests that the dbHash command acquires IX mode locks on the global, database, and collection
 * resources when running inside a multi-statement transaction.
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const db = primary.getDB("test");

    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(db.getName());

    function assertTransactionAcquiresIXLocks(txnOptions) {
        session.startTransaction(txnOptions);

        assert.commandWorked(sessionDB.runCommand({dbHash: 1}));
        const ops = db.currentOp({"lsid.id": session.getSessionId().id}).inprog;
        assert.eq(1,
                  ops.length,
                  () => "Failed to find session in currentOp() output: " + tojson(db.currentOp()));
        assert.eq(ops[0].locks, {Global: "w", Database: "w", Collection: "w"});

        session.abortTransaction();
    }

    // We insert a document so the dbHash command has a collection to process.
    assert.commandWorked(db.mycoll.insert({}, {writeConcern: {w: "majority"}}));

    assertTransactionAcquiresIXLocks({});
    assertTransactionAcquiresIXLocks({readConcern: {level: "local"}});
    assertTransactionAcquiresIXLocks({readConcern: {level: "snapshot"}});
    assertTransactionAcquiresIXLocks({
        readConcern: {
            level: "snapshot",
            atClusterTime: session.getOperationTime(),
        }
    });

    session.endSession();
    rst.stopSet();
})();
