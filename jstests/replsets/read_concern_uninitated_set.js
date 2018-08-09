/**
 * Test to ensure that specifying non-local read concern with an uninitiated set does not crash
 * node.
 *
 * @tags: [requires_persistence, requires_majority_read_concern]
 */
(function() {
    "use strict";
    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();

    // We initiate the replset normally, then delete the config document and restart the node back
    // into an uninitiated state.  This is because we can't set the cluster time until the feature
    // compatibility version is 3.6, and we can't set the feature compatibility version until we're
    // primary.  Initiating the replset automatically sets FCV to 3.6.
    rst.initiate();
    var localDB = rst.nodes[0].getDB('local');
    assert.writeOK(localDB.system.replset.remove({}));
    rst.restart(rst.nodes[0]);
    localDB = rst.nodes[0].getDB('local');

    assert.writeOK(localDB.test.insert({_id: 0}));
    assert.commandWorked(localDB.runCommand({
        isMaster: 1,
        "$clusterTime": {
            "clusterTime": Timestamp(1, 1),
            "signature":
                {"hash": BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAA="), "keyId": NumberLong(0)}
        }
    }));
    jsTestLog("Local readConcern on local database should work.");
    const res = assert.commandWorked(localDB.runCommand(
        {find: "test", filter: {}, maxTimeMS: 60000, readConcern: {level: "local"}}));
    assert.eq([{_id: 0}], res.cursor.firstBatch);

    jsTestLog("Majority readConcern should fail with NotYetInitialized.");
    assert.commandFailedWithCode(
        localDB.runCommand(
            {find: "test", filter: {}, maxTimeMS: 60000, readConcern: {level: "majority"}}),
        ErrorCodes.NotYetInitialized);

    jsTestLog("afterClusterTime readConcern should fail with NotYetInitialized.");
    assert.commandFailedWithCode(localDB.runCommand({
        find: "test",
        filter: {},
        maxTimeMS: 60000,
        readConcern: {afterClusterTime: Timestamp(1, 1)}
    }),
                                 ErrorCodes.NotYetInitialized);

    jsTestLog("oplog query should fail with NotYetInitialized.");
    assert.commandFailedWithCode(localDB.runCommand({
        find: "oplog.rs",
        filter: {ts: {$gte: Timestamp(1520004466, 2)}},
        tailable: true,
        oplogReplay: true,
        awaitData: true,
        maxTimeMS: 60000,
        batchSize: 13981010,
        term: 1,
        readConcern: {afterClusterTime: Timestamp(1, 1)}
    }),
                                 ErrorCodes.NotYetInitialized);
    rst.stopSet();
}());
