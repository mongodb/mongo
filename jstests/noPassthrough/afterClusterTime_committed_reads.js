// Test that causally consistent majority-committed reads will wait for the majority commit point to
// move past 'afterClusterTime'.
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

    function testReadConcernLevel(level) {
        // Stop replication.
        stopReplicationOnSecondaries(rst);

        // Perform a write and get its op time.
        const res = assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{}]}));
        assert(res.hasOwnProperty("opTime"), tojson(res));
        assert(res.opTime.hasOwnProperty("ts"), tojson(res));
        const clusterTime = res.opTime.ts;

        // A committed read on the primary after the new cluster time should time out waiting for
        // the cluster time to be majority committed.
        session.startTransaction({readConcern: {level: level, afterClusterTime: clusterTime}});
        assert.commandFailedWithCode(primaryDB.runCommand({find: collName, maxTimeMS: 1000}),
                                     ErrorCodes.ExceededTimeLimit);
        session.abortTransaction();

        // Restart replication.
        restartReplicationOnSecondaries(rst);

        // A committed read on the primary after the new cluster time now succeeds.
        session.startTransaction({readConcern: {level: level, afterClusterTime: clusterTime}});
        assert.commandWorked(primaryDB.runCommand({find: collName}));
        session.commitTransaction();
    }

    if (assert.commandWorked(primaryDB.serverStatus()).storageEngine.supportsCommittedReads) {
        testReadConcernLevel("majority");
    }

    if (assert.commandWorked(primaryDB.serverStatus()).storageEngine.supportsSnapshotReadConcern) {
        testReadConcernLevel("snapshot");
    }

    rst.stopSet();
}());
