/**
 * Test that commitTransaction and abortTransaction commands work on replica set secondaries.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "transaction_commit_abort_on_secondaries";

    const rst = new ReplSetTest({name: collName, nodes: 2});
    rst.startSet();
    // We want a stable topology, so make the secondary unelectable.
    let config = rst.getReplSetConfig();
    config.members[1].priority = 0;
    rst.initiate(config);

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const secondaryTestDB = secondary.getDB(dbName);

    // Do an initial write so we have something to find.
    const initialDoc = {_id: 0};
    assert.writeOK(primary.getDB(dbName)[collName].insert(initialDoc));
    rst.awaitLastOpCommitted();

    // Initiate a session on the secondary.
    const sessionOptions = {causalConsistency: false};
    const session = secondaryTestDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    /**
     * Test commitTransaction.
     */

    jsTestLog("Start a read-only transaction on the secondary.");
    session.startTransaction({readConcern: {level: "snapshot"}});

    // Read a document.
    assert.eq(initialDoc, sessionDb[collName].findOne({}));

    jsTestLog("Make sure we can commit the transaction on the secondary.");
    session.commitTransaction();

    /**
     * Test abortTransaction.
     */

    jsTestLog("Start a read-only transaction on the secondary.");
    session.startTransaction({readConcern: {level: "snapshot"}});

    // Read a document.
    assert.eq(initialDoc, sessionDb[collName].findOne({}));

    jsTestLog("Make sure we can abort the transaction on the secondary.");
    session.abortTransaction();

    session.endSession();
    rst.stopSet();
}());
