// Test that 'atClusterTime' is used to select the snapshot for reads. We wait for 'atClusterTime'
// to be majority committed. If 'atClusterTime' is older than the oldest available snapshot, the
// error code SnapshotTooOld is returned.
//
// @tags: [uses_transactions, requires_majority_read_concern]
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // For stopServerReplication.

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 3, settings: {chainingAllowed: false}});
    rst.startSet();
    rst.initiate();

    const primarySession =
        rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    const primaryDB = primarySession.getDatabase(dbName);

    const secondaryConns = rst.getSecondaries();
    const secondaryConn0 = secondaryConns[0];
    const secondaryConn1 = secondaryConns[1];
    const secondarySession =
        secondaryConn0.getDB(dbName).getMongo().startSession({causalConsistency: false});
    const secondaryDB0 = secondarySession.getDatabase(dbName);

    // Create the collection and insert one document. Get the op time of the write.
    let res = assert.commandWorked(primaryDB.runCommand(
        {insert: collName, documents: [{_id: "before"}], writeConcern: {w: "majority"}}));
    const clusterTimePrimaryBefore = res.opTime.ts;

    // Wait for the majority commit point on 'secondaryDB0' to include the {_id: "before"} write.
    assert.soonNoExcept(function() {
        return assert
                   .commandWorked(secondaryDB0.runCommand(
                       {find: collName, readConcern: {level: "majority"}, maxTimeMS: 10000}))
                   .cursor.firstBatch.length === 1;
    });

    // Stop replication on both secondaries.
    stopServerReplication(secondaryConn0);
    stopServerReplication(secondaryConn1);

    // Perform write and get the op time of the write.
    res =
        assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: "after"}]}));
    assert(res.hasOwnProperty("opTime"), tojson(res));
    assert(res.opTime.hasOwnProperty("ts"), tojson(res));
    let clusterTimeAfter = res.opTime.ts;

    // A read on the primary at the old cluster time should not include the write.
    primarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimePrimaryBefore}});
    res = assert.commandWorked(primaryDB.runCommand({find: collName}));
    assert.commandWorked(primarySession.commitTransaction_forTesting());
    assert.eq(res.cursor.firstBatch.length, 1, printjson(res));
    assert.eq(res.cursor.firstBatch[0]._id, "before", printjson(res));

    // A read on the primary at the new cluster time should succeed because transactions implement
    // speculative behavior, but the attempt to commit the transaction should time out waiting for
    // the transaction to be majority committed.
    primarySession.startTransaction({
        readConcern: {level: "snapshot", atClusterTime: clusterTimeAfter},
        writeConcern: {w: "majority", wtimeout: 1000}
    });
    res = assert.commandWorked(primaryDB.runCommand({find: collName}));
    assert.eq(res.cursor.firstBatch.length, 2, printjson(res));
    assert.commandFailedWithCode(primarySession.commitTransaction_forTesting(),
                                 ErrorCodes.WriteConcernFailed);

    // A read on the primary at the new cluster time succeeds.
    primarySession.startTransaction({
        readConcern: {level: "snapshot", atClusterTime: clusterTimeAfter},
        writeConcern: {w: "majority"}
    });
    res = assert.commandWorked(primaryDB.runCommand({find: collName}));
    assert.eq(res.cursor.firstBatch.length, 2, printjson(res));
    // Restart replication on one of the secondaries.
    restartServerReplication(secondaryConn1);
    // This time the transaction should commit.
    assert.commandWorked(primarySession.commitTransaction_forTesting());

    // Restart replication on the lagged secondary.
    restartServerReplication(secondaryConn0);

    // A read at a time that is too old fails.
    primarySession.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: Timestamp(1, 1)}});
    assert.commandFailedWithCode(primaryDB.runCommand({find: collName}), ErrorCodes.SnapshotTooOld);
    assert.commandFailedWithCode(primarySession.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    rst.stopSet();
}());
