// Test TransientTransactionError error label for commands in transactions with write concern.
// @tags: [uses_transactions]
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");
    load("jstests/replsets/rslib.js");

    const dbName = "test";
    const collName = "transient_txn_error_labels_with_write_concern";
    const rst = new ReplSetTest({name: collName, nodes: 3});
    const config = rst.getReplSetConfig();
    config.members[2].priority = 0;
    config.settings = {};
    // Disable catchup so the new primary will not sync from the old one.
    config.settings.catchUpTimeoutMillis = 0;
    // Disable catchup takeover to prevent the old primary to take over the new one.
    config.settings.catchUpTakeoverDelayMillis = -1;
    rst.startSet();
    rst.initiate(config);

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    assert.eq(primary, rst.nodes[0]);
    const testDB = primary.getDB(dbName);

    const sessionOptions = {causalConsistency: false};
    const writeConcernMajority = {w: "majority", wtimeout: 500};

    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    jsTest.log("Write concern errors should not have error labels");
    // Start a new session on the primary.
    let session = primary.startSession(sessionOptions);
    let sessionDb = session.getDatabase(dbName);
    let sessionColl = sessionDb.getCollection(collName);
    stopServerReplication(rst.getSecondaries());
    session.startTransaction({writeConcern: writeConcernMajority});
    assert.commandWorked(sessionColl.insert({_id: "write-with-write-concern"}));
    let res = session.commitTransaction_forTesting();
    checkWriteConcernTimedOut(res);
    assert(!res.hasOwnProperty("code"));
    assert(!res.hasOwnProperty("errorLabels"));
    restartServerReplication(rst.getSecondaries());

    jsTest.log(
        "commitTransaction should wait for write concern even if it returns NoSuchTransaction");
    // Make sure the new primary only miss the commitTransaction sent in this test case.
    rst.awaitReplication();

    session.startTransaction(writeConcernMajority);
    // Pick up a high enough txnNumber so that it doesn't conflict with previous test cases.
    let txnNumber = 20;
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "commitTransaction-with-write-concern"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    const oldPrimary = rst.getPrimary();
    // Stop replication on all nodes, including the old primary so that it won't replicate from the
    // new primary.
    stopServerReplication(rst.nodes);
    // commitTransaction fails on the old primary.
    res = sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: writeConcernMajority,
    });
    checkWriteConcernTimedOut(res);

    // Failover happens, but the new secondary cannot replicate anything to others.
    assert.commandWorked(secondary.adminCommand({replSetStepUp: 1}));
    rst.awaitNodesAgreeOnPrimary();
    reconnect(secondary);
    // Restart replication on the new primary, so it can become "master".
    restartServerReplication(secondary);
    const newPrimary = rst.getPrimary();
    assert.neq(oldPrimary, newPrimary);
    const newPrimarySession = newPrimary.startSession(sessionOptions);
    // Force the new session to use the old session id to simulate driver's behavior.
    const overridenSessionId = newPrimarySession._serverSession.handle.getId();
    const lsid = session._serverSession.handle.getId();
    jsTest.log("Overriding sessionID " + tojson(overridenSessionId) + " with " + tojson(lsid));
    newPrimarySession._serverSession.handle.getId = () => lsid;
    const newPrimarySessionDb = newPrimarySession.getDatabase(dbName);

    res = newPrimarySessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: writeConcernMajority,
    });
    checkWriteConcernTimedOut(res);
    assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);

    jsTest.log("NoSuchTransaction with write concern error is not transient");
    assert(!res.hasOwnProperty("errorLabels"));

    jsTest.log("NoSuchTransaction without write concern error is transient");
    restartServerReplication(rst.nodes);
    res = newPrimarySessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},  // Wait with a long timeout.
    });
    assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
    assert(!res.hasOwnProperty("writeConcernError"), res);
    assert.eq(res["errorLabels"], ["TransientTransactionError"], res);

    rst.awaitNodesAgreeOnPrimary();
    session.endSession();

    rst.stopSet();
}());
