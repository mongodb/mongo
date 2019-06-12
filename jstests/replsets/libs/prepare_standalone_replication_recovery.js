"use strict";

/*
 * Library used to test that we can recover prepared transactions using the
 * 'recoverFromOplogAsStandalone' flag. This can be used to test with both prepared transactions
 * that have been committed and ones that are still in the prepared state.
 */

var testPrepareRecoverFromOplogAsStandalone = function(name, commitBeforeRecovery) {
    load("jstests/replsets/rslib.js");
    load("jstests/libs/write_concern_util.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const txnCollName = "txn_coll";
    const nonTxnCollName = "non_txn_coll";
    const logLevel = tojson({storage: {recovery: 3}});

    const rst = new ReplSetTest({
        name: name,
        nodes: 2,
    });

    function getColl(conn, collName = nonTxnCollName) {
        return conn.getDB(dbName)[collName];
    }

    function assertDocsInColl(conn, nums, collName) {
        const results = getColl(conn, collName).find().sort({_id: 1}).toArray();
        const expected = nums.map((i) => ({_id: i}));
        if (!friendlyEqual(results, expected)) {
            rst.dumpOplog(node, {}, 100);
        }
        assert.eq(results, expected, "actual (left) != expected (right)");
    }

    jsTestLog("Initiating as a replica set.");
    // Start up the nodes as a replica set so we can add operations to the oplog.
    const nodes = rst.startSet({setParameter: {logComponentVerbosity: logLevel}});
    let node = nodes[0];
    const secondary = nodes[1];
    rst.initiate({
        _id: name,
        members: [{_id: 0, host: node.host}, {_id: 2, host: secondary.host, priority: 0}]
    });

    assert.eq(rst.getPrimary(), node);

    // Create both collections with {w: majority}.
    assert.commandWorked(node.getDB(dbName).runCommand({
        create: nonTxnCollName,
        writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS}
    }));
    assert.commandWorked(node.getDB(dbName).runCommand({
        create: txnCollName,
        writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS}
    }));

    jsTestLog("Beginning a transaction.");
    const session = node.startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(txnCollName);

    // Start the transaction and insert some documents.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 1234}));
    assert.commandWorked(sessionColl.insert({_id: 5678}));

    // Prepare the transaction.
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // Keep node 0 the primary, but prevent it from committing any writes.
    stopServerReplication(secondary);

    // Issue a few non-transactional writes that will not be majority-committed.
    assert.commandWorked(getColl(node).insert({_id: 3}));
    assert.commandWorked(getColl(node).insert({_id: 4}));
    assert.commandWorked(getColl(node).insert({_id: 5}));
    assertDocsInColl(node, [3, 4, 5]);

    if (commitBeforeRecovery) {
        // Also commit the prepared transaction, but only on the primary.
        jsTestLog("Committing the transaction (before recovery).");
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
    }

    jsTestLog("Testing that on restart with the flag set we play recovery.");
    // TODO SERVER-41888: Never skip collection validation.
    node = rst.restart(node, {
        noReplSet: true,
        skipValidation: !commitBeforeRecovery,
        setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}
    });

    reconnect(node);
    assertDocsInColl(node, [3, 4, 5]);

    // Verify that we can only see the contents of the transaction if it was committed.
    const expectedDocs = commitBeforeRecovery ? [1234, 5678] : [];
    assertDocsInColl(node, expectedDocs, txnCollName);

    // TODO SERVER-41888: Never skip collection validation.
    rst.stop(node, undefined, {skipValidation: true});
    rst.stop(secondary, undefined, {skipValidation: true});
};
