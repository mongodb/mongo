/**
 * Test unfinished 4.0-style two phase drop can be handled in a restart with 4.2 binary.
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
    rst.startSet({binVersion: "4.0"});
    rst.initiate();

    const dbName = "test";
    const collName = "a";
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const testDB = primary.getDB(dbName);

    // Make sure collection creation is checkpointed.
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [{x: 0}]}));
    assert.commandWorked(primary.getDB("admin").runCommand({fsync: 1}));

    // Stop secondary's oplog application so the dropCollection can never be committed.
    assert.commandWorked(
        secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));
    assert.commandWorked(testDB.runCommand({drop: collName}));

    // Wait until the first phase (renaming) is done on the primary.
    assert.soon(function() {
        let res = primary.getDB("local").oplog.rs.find({o: {drop: collName}}).toArray();
        jsTestLog("dropCollection oplog: " + tojson(res));
        return res.length === 1;
    });
    // This will print out 'test.system.drop.xxxxxx.a' collection if there is one.
    assert(TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(testDB, collName));

    // Kill the 4.0 replica set.
    rst.stopSet(9 /* signal */,
                true /* forRestart */,
                {skipValidation: true, allowedExitCode: MongoRunner.EXIT_SIGKILL});

    // Restart the replica set with 4.2 binaries.
    rst.startSet({restart: true, binVersion: "latest"});

    assert.soon(function() {
        if (!TestData.hasOwnProperty("enableMajorityReadConcern") ||
            TestData.enableMajorityReadConcern === true) {
            // If enableMajorityReadConcern is true, that means the binary will use the new
            // 4.2-style two phase drop. Then there should never be 'test.system.drop.xxxxx.a' in
            // 'test' database because the first phase of 4.0-style drop (rename) was not
            // checkpointed and that drop is currently being replayed via 4.2-style two phase drop
            // mechanism.
            assert(!TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(
                rst.getPrimary().getDB(dbName), collName));
        }
        let res = TwoPhaseDropCollectionTest.listCollections(rst.getPrimary().getDB(dbName));
        jsTestLog("Collections in \'" + dbName + "\': " + tojson(res));
        return res.length === 0;
    });

    rst.stopSet();
})();
