/**
 * Tests that if an initial syncing node runs an index build and a conflicting DDL operation
 * at the same time during the oplog application phase, it will not abort single phase index builds.
 *
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");
load("jstests/noPassthrough/libs/index_build.js");

function runTest(runDbCommand) {
    const name = jsTestName();
    // Disable two phase index builds so the replica set only runs single phase index builds.
    const rst = new ReplSetTest({
        name,
        nodes: 1,
        nodeOptions: {setParameter: "enableTwoPhaseIndexBuild=0"},
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const primary = rst.getPrimary();
    const dbName = jsTestName();
    const collName = "coll";
    const primaryDb = primary.getDB(dbName);
    const primaryColl = primaryDb.getCollection(collName);
    const collFullName = primaryColl.getFullName();

    // Insert initial data to ensure that the repl set is initialized correctly.
    assert.commandWorked(primaryColl.insert({a: 1}));
    rst.awaitReplication();

    jsTestLog("Adding a new node to the replica set");
    const initialSyncNode = rst.add({});

    const hangAfterDataCloning =
        configureFailPoint(initialSyncNode, "initialSyncHangAfterDataCloning");
    const indexHang = configureFailPoint(initialSyncNode, "hangAfterStartingIndexBuild");

    jsTestLog("Waiting for initial sync node to reach initial sync state");
    rst.reInitiate();
    rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

    // Hang the initial sync node after the data cloning phase to ensure that the node will
    // apply the index build oplog entry as part of the oplog application phase of initial sync.
    hangAfterDataCloning.wait();

    jsTestLog("Creating index build and running DDL operation on primary");
    primaryColl.createIndex({a: 1});

    let awaitDropDatabase;
    if (runDbCommand) {
        TestData.dbName = dbName;
        awaitDropDatabase = startParallelShell(() => {
            assert.commandWorked(db.getSiblingDB(TestData.dbName).dropDatabase());
        }, primary.port);
        checkLog.containsJson(primary, 20337);
    } else {
        assert.commandWorked(primaryColl.renameCollection('coll2'));
    }

    jsTestLog("Continuing with initial sync and hanging on index build");
    hangAfterDataCloning.off();

    // Hang the index build after starting so that a DDL operation
    // will conflict with the index build, which will attempt to abort two phase index builds.
    indexHang.wait();

    // Both the dropDatabase command and the renameCollection command will try to abort
    // collection index builds because when the primary receives a dropDatabase command, it first
    // drops collections, and on the secondary a dropCollection command is actually run before
    // the dropDatabase command.
    checkLog.containsJson(
        initialSyncNode,
        5500800,
        {reason: "Aborting two phase index builds during initial sync", namespace: collFullName});

    jsTestLog("Wait for the initial sync to succeed");
    indexHang.off();
    waitForState(initialSyncNode, ReplSetTest.State.SECONDARY);

    jsTestLog("Check that the single phase index build should have successfully been built");
    if (runDbCommand) {
        // Check that the log contains line indicating that the index build has successfully
        // completed.
        checkLog.containsJson(initialSyncNode, 20663, {"namespace": collFullName});
        awaitDropDatabase();
    } else {
        IndexBuildTest.assertIndexes(
            initialSyncNode.getDB(dbName).getCollection("coll2"), 2, ["_id_", "a_1"]);
    }

    rst.stopSet();
}

runTest(false);
runTest(true);
})();
