// Tests the 'lastCommittedTransaction' serverStatus section.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const dbName = "test";
    const collName = "coll";

    const sessionOptions = {causalConsistency: false};
    const session = primary.getDB(dbName).getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];
    assert.commandWorked(sessionDb.runCommand({create: collName}));

    function checkLastCommittedTransaction(operationCount, writeConcern) {
        let res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        assert(res.hasOwnProperty("transactions"), tojson(res));
        assert(res.transactions.hasOwnProperty("lastCommittedTransaction"),
               tojson(res.transactions));
        assert.eq(operationCount,
                  res.transactions.lastCommittedTransaction.operationCount,
                  tojson(res.transactions));
        if (operationCount === 0) {
            assert.eq(0,
                      res.transactions.lastCommittedTransaction.oplogOperationBytes,
                      tojson(res.transactions));
        } else {
            assert.lt(0,
                      res.transactions.lastCommittedTransaction.oplogOperationBytes,
                      tojson(res.transactions));
        }
        assert.docEq(writeConcern,
                     res.transactions.lastCommittedTransaction.writeConcern,
                     tojson(res.transactions));
    }

    // Initially the 'lastCommittedTransaction' section is not present.
    let res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    assert(res.hasOwnProperty("transactions"), tojson(res));
    assert(!res.transactions.hasOwnProperty("lastCommittedTransaction"), tojson(res));

    // Start a transaction. The 'lastCommittedTransaction' section is not updated until the
    // transaction commits.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({}));
    res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    assert(res.hasOwnProperty("transactions"), tojson(res));
    assert(!res.transactions.hasOwnProperty("lastCommittedTransaction"), tojson(res));

    // Commit the transaction. The 'lastCommittedTransaction' section should be updated.
    assert.commandWorked(session.commitTransaction_forTesting());
    checkLastCommittedTransaction(1, {});

    // Run a transaction with multiple write operations.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({}));
    assert.commandWorked(sessionColl.insert({}));
    assert.commandWorked(session.commitTransaction_forTesting());
    checkLastCommittedTransaction(2, {});

    // Run a read-only transaction.
    session.startTransaction();
    sessionColl.findOne();
    assert.commandWorked(session.commitTransaction_forTesting());
    checkLastCommittedTransaction(0, {});

    // Run a transaction with non-default writeConcern.
    session.startTransaction({writeConcern: {w: 1}});
    assert.commandWorked(sessionColl.insert({}));
    assert.commandWorked(session.commitTransaction_forTesting());
    checkLastCommittedTransaction(1, {w: 1, wtimeout: 0});

    // Run a read-only transaction with non-default writeConcern.
    session.startTransaction({writeConcern: {w: "majority"}});
    sessionColl.findOne();
    assert.commandWorked(session.commitTransaction_forTesting());
    checkLastCommittedTransaction(0, {w: "majority", wtimeout: 0});

    session.endSession();
    rst.stopSet();
}());
