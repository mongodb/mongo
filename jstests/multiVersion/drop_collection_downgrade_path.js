/**
 * Test unfinished 4.2-style two phase drop can be handled in a restart with 4.0 binary.
 *
 * TODO: Remove this test in MongoDB 4.4 once upgrade & downgrade binaries both use the 4.2-style
 * two phase drop.
 */
(function() {
    "use strict";

    TestData.skipCheckDBHashes = true;  // Skip db hashes when restarting the replset.
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/replsets/libs/two_phase_drops.js");  // For 'TwoPhaseDropCollectionTest'.

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = "a";
    assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    rst.stopSet(null, true /* forRestart */);
    rst.startSet({restart: true});

    // Make sure collection creation is checkpointed.
    assert.commandWorked(rst.getPrimary().getDB(dbName).runCommand(
        {insert: collName, documents: [{x: 0}], writeConcern: {w: 2}}));
    assert.commandWorked(rst.getPrimary().getDB("admin").runCommand({fsync: 1}));

    // Stop secondary's oplog application so the dropCollection can never be committed.
    assert.commandWorked(
        rst.getSecondary().adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));
    assert.commandWorked(rst.getPrimary().getDB(dbName).runCommand({drop: collName}));

    // Wait until the first phase (renaming) is done on the primary.
    assert.soon(function() {
        let res = rst.getPrimary().getDB("local").oplog.rs.find({o: {drop: collName}}).toArray();
        jsTestLog("dropCollection oplog: " + tojson(res));
        return res.length === 1;
    });

    // Kill the 4.2 replica set.
    rst.stopSet(9 /* signal */,
                true /* forRestart */,
                {skipValidation: true, allowedExitCode: MongoRunner.EXIT_SIGKILL});

    // Restart the replica set with 4.0 binaries.
    rst.startSet({restart: true, binVersion: "4.0"});

    assert.soon(function() {
        let res = TwoPhaseDropCollectionTest.listCollections(rst.getPrimary().getDB(dbName));
        jsTestLog("Collections in \'" + dbName + "\': " + tojson(res));
        return res.length === 0;
    });

    rst.stopSet();
})();
