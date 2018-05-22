// Test that causally consistent majority-committed read-only transactions will wait for the
// majority commit point to move past 'afterClusterTime' before they can commit.
// @tags: [requires_replication]
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const session =
        rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    const primaryDB = session.getDatabase(dbName);

    let txnNumber = 0;

    function testReadConcernLevel(level) {
        // Stop replication.
        stopReplicationOnSecondaries(rst);

        // Perform a write and get its op time.
        const res = assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{}]}));
        assert(res.hasOwnProperty("opTime"), tojson(res));
        assert(res.opTime.hasOwnProperty("ts"), tojson(res));
        const clusterTime = res.opTime.ts;

        // A majority-committed read-only transaction on the primary after the new cluster time
        // should time out at commit time waiting for the cluster time to be majority committed.
        assert.commandWorked(primaryDB.runCommand({
            find: collName,
            txnNumber: NumberLong(++txnNumber),
            startTransaction: true,
            autocommit: false,
            readConcern: {level: level, afterClusterTime: clusterTime}
        }));
        assert.commandFailedWithCode(primaryDB.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"},
            maxTimeMS: 1000
        }),
                                     ErrorCodes.ExceededTimeLimit);

        // Restart replication.
        restartReplicationOnSecondaries(rst);

        // A majority-committed read-only transaction on the primary after the new cluster time now
        // succeeds.
        assert.commandWorked(primaryDB.runCommand({
            find: collName,
            txnNumber: NumberLong(++txnNumber),
            startTransaction: true,
            autocommit: false,
            readConcern: {level: level, afterClusterTime: clusterTime}
        }));
        assert.commandWorked(primaryDB.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
            writeConcern: {w: "majority"}
        }));
    }

    if (assert.commandWorked(primaryDB.serverStatus()).storageEngine.supportsCommittedReads) {
        testReadConcernLevel("majority");
    }

    if (assert.commandWorked(primaryDB.serverStatus()).storageEngine.supportsSnapshotReadConcern) {
        testReadConcernLevel("snapshot");
    }

    rst.stopSet();
}());
