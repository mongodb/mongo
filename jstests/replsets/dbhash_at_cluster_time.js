/**
 * Tests that "atClusterTime" is supported by the "dbHash" command.
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

    if (!db.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    // We force 'secondary' to sync from 'primary' using the "forceSyncSourceCandidate" failpoint to
    // ensure that an intermittent connectivity issue doesn't lead to the secondary not advancing
    // its belief of the majority commit point. This avoids any complications that would arise due
    // to SERVER-33248.
    assert.commandWorked(secondary.adminCommand({
        configureFailPoint: "forceSyncSourceCandidate",
        mode: "alwaysOn",
        data: {hostAndPort: primary.host}
    }));
    rst.awaitSyncSource(secondary, primary);

    // We also prevent all nodes in the replica set from advancing oldest_timestamp. This ensures
    // that the snapshot associated with 'clusterTime' is retained for the duration of this test.
    rst.nodes.forEach(conn => {
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
            mode: "alwaysOn",
        }));
    });

    // We insert a document and save the md5sum associated with the opTime of that write.
    assert.commandWorked(db.mycoll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    const clusterTime = db.getSession().getOperationTime();

    session.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
    let res = assert.commandWorked(db.runCommand({dbHash: 1}));
    session.commitTransaction();
    const hash1 = {collections: res.collections, md5: res.md5};

    // We insert another document to ensure the collection's contents have a different md5sum now.
    assert.commandWorked(db.mycoll.insert({_id: 2}));

    // However, using atClusterTime to read at the opTime of the first insert should return the same
    // md5sum as it did originally.
    session.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
    res = assert.commandWorked(db.runCommand({dbHash: 1}));
    session.commitTransaction();
    const hash2 = {collections: res.collections, md5: res.md5};
    assert.eq(hash1, hash2, "primary returned different dbhash after second insert");

    {
        const secondarySession = secondary.startSession({causalConsistency: false});
        const secondaryDB = secondarySession.getDatabase("test");

        // Using atClusterTime to read at the opTime of the first insert should return the same
        // md5sum on the secondary as it did on the primary.
        secondarySession.startTransaction(
            {readConcern: {level: "snapshot", atClusterTime: clusterTime}});
        res = assert.commandWorked(secondaryDB.runCommand({dbHash: 1}));
        secondarySession.commitTransaction();
        const secondaryHash = {collections: res.collections, md5: res.md5};
        assert.eq(hash1, secondaryHash, "primary and secondary have different dbhash");

        secondarySession.endSession();
    }

    {
        const otherSession = primary.startSession({causalConsistency: false});
        const otherDB = otherSession.getDatabase("test");

        // We perform another insert inside a separate transaction to cause a MODE_IX lock to be
        // held on the collection.
        otherSession.startTransaction();
        assert.commandWorked(otherDB.mycoll.insert({_id: 3}));

        // It should be possible to run the "dbHash" command with "atClusterTime" concurrently.
        session.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
        res = assert.commandWorked(db.runCommand({dbHash: 1}));
        session.commitTransaction();
        const hash3 = {collections: res.collections, md5: res.md5};
        assert.eq(hash1, hash3, "primary returned different dbhash after third insert");

        // However, the "dbHash" command should block behind the transaction if "atClusterTime"
        // wasn't specified.
        res = assert.commandFailedWithCode(db.runCommand({dbHash: 1, maxTimeMS: 1000}),
                                           ErrorCodes.ExceededTimeLimit);

        otherSession.abortTransaction();
        otherSession.endSession();
    }

    {
        const otherSession = primary.startSession({causalConsistency: false});
        const otherDB = otherSession.getDatabase("test");

        // We create another collection inside a separate session to modify the collection catalog
        // at an opTime later than 'clusterTime'. This prevents further usage of the snapshot
        // associated with 'clusterTime' for snapshot reads.
        assert.commandWorked(otherDB.runCommand({create: "mycoll2"}));
        session.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
        assert.commandFailedWithCode(db.runCommand({dbHash: 1}), ErrorCodes.SnapshotUnavailable);
        session.abortTransaction();

        otherSession.endSession();
    }

    session.endSession();
    rst.stopSet();
})();
