/**
 * Tests that "$_internalReadAtClusterTime" is supported by the "dbHash" command.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();

    const replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    rst.initiate(replSetConfig);

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    const session = primary.startSession({causalConsistency: false});
    const db = session.getDatabase("test");
    let txnNumber = 0;

    // We prevent the replica set from advancing oldest_timestamp. This ensures that the snapshot
    // associated with 'clusterTime' is retained for the duration of this test.
    rst.nodes.forEach(conn => {
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
            mode: "alwaysOn",
        }));
    });

    // We insert a document and save the md5sum associated with the opTime of that write.
    assert.commandWorked(db.mycoll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    const clusterTime = db.getSession().getOperationTime();

    let res = assert.commandWorked(db.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: clusterTime,
    }));

    const hash1 = {collections: res.collections, md5: res.md5};

    // We insert another document to ensure the collection's contents have a different md5sum now.
    // We use a w=majority write concern to ensure that the insert has also been applied on the
    // secondary by the time we go to run the dbHash command later. This avoids a race where the
    // replication subsystem could be applying the insert operation when the dbHash command is run
    // on the secondary.
    assert.commandWorked(db.mycoll.insert({_id: 2}, {writeConcern: {w: "majority"}}));

    // However, using $_internalReadAtClusterTime to read at the opTime of the first insert should
    // return the same md5sum as it did originally.
    res = assert.commandWorked(db.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: clusterTime,
    }));

    const hash2 = {collections: res.collections, md5: res.md5};
    assert.eq(hash1, hash2, "primary returned different dbhash after second insert");

    {
        const secondarySession = secondary.startSession({causalConsistency: false});
        const secondaryDB = secondarySession.getDatabase("test");

        // Using $_internalReadAtClusterTime to read at the opTime of the first insert should return
        // the same md5sum on the secondary as it did on the primary.
        res = assert.commandWorked(secondaryDB.runCommand({
            dbHash: 1,
            $_internalReadAtClusterTime: clusterTime,
        }));

        const secondaryHash = {collections: res.collections, md5: res.md5};
        assert.eq(hash1, secondaryHash, "primary and secondary have different dbhash");
    }

    {
        const otherSession = primary.startSession({causalConsistency: false});
        const otherDB = otherSession.getDatabase("test");

        // We perform another insert inside a separate transaction to cause a MODE_IX lock to be
        // held on the collection.
        otherSession.startTransaction();
        assert.commandWorked(otherDB.mycoll.insert({_id: 3}));

        // It should be possible to run the "dbHash" command with "$_internalReadAtClusterTime"
        // concurrently.
        res = assert.commandWorked(db.runCommand({
            dbHash: 1,
            $_internalReadAtClusterTime: clusterTime,
        }));

        const hash3 = {collections: res.collections, md5: res.md5};
        assert.eq(hash1, hash3, "primary returned different dbhash after third insert");

        // However, the "dbHash" command should block behind the transaction if
        // "$_internalReadAtClusterTime" wasn't specified.
        res = assert.commandFailedWithCode(db.runCommand({dbHash: 1, maxTimeMS: 1000}),
                                           ErrorCodes.MaxTimeMSExpired);

        assert.commandWorked(otherSession.abortTransaction_forTesting());
        otherSession.endSession();
    }

    {
        const otherSession = primary.startSession({causalConsistency: false});
        const otherDB = otherSession.getDatabase("test");

        // We create another collection inside a separate session to modify the collection catalog
        // at an opTime later than 'clusterTime'. This prevents further usage of the snapshot
        // associated with 'clusterTime' for snapshot reads.
        assert.commandWorked(otherDB.runCommand({create: "mycoll2"}));
        assert.commandFailedWithCode(
            db.runCommand({dbHash: 1, $_internalReadAtClusterTime: clusterTime}),
            ErrorCodes.SnapshotUnavailable);

        otherSession.endSession();
    }

    session.endSession();
    rst.stopSet();
})();
