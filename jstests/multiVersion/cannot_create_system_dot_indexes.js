// Tests that the 'system.indexes' collection cannot be created via data transfer from a
// lower-version node.
(function() {

    // This test should not be run on mmapv1 because the 'system.indexes' collection exists on that
    // storage engine.
    const isMMAPv1 = jsTest.options().storageEngine === "mmapv1";
    if (isMMAPv1) {
        return;
    }

    const latest = "latest";
    const downgrade = "3.6";
    const downgradeFCV = "3.6";

    const dbName = "test";
    const collName = "coll";

    const upgradeConn = MongoRunner.runMongod({binVersion: latest});
    const upgradeDB = upgradeConn.getDB(dbName);

    // Set the featureCompatibilityVersion to the downgrade value to allow data transfer from a
    // lower-version node.
    assert.commandWorked(upgradeDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    const downgradeConn = MongoRunner.runMongod({binVersion: downgrade});
    const downgradeDB = downgradeConn.getDB(dbName);

    // The 'system.indexes' collection can be created on a lower-version node.
    assert.commandWorked(downgradeDB.coll.insert({}));
    assert.commandWorked(downgradeDB.createCollection("system.indexes"));
    assert.eq(1,
              downgradeDB.getCollectionInfos({name: "system.indexes"}).length,
              tojson(downgradeDB.getCollectionInfos()));

    // The 'system.indexes' collection cannot be created on the upgrade node using
    // 'cloneCollection'.
    assert.commandFailedWithCode(
        upgradeDB.cloneCollection(downgradeDB.getMongo().host, "system.indexes"),
        ErrorCodes.InvalidNamespace);

    // The 'clone' command does not copy the 'system.indexes' collection.
    assert.commandWorked(upgradeDB.cloneDatabase(downgradeDB.getMongo().host));
    assert.eq(0,
              upgradeDB.getCollectionInfos({name: "system.indexes"}).length,
              tojson(upgradeDB.getCollectionInfos()));

    // The 'copydb' command does not copy the 'system.indexes' collection.
    if (jsTest.options().auth) {
        downgradeDB.createUser({
            user: jsTest.options().authUser,
            pwd: jsTest.options().authPassword,
            roles: ["dbOwner"]
        });
        assert.commandWorked(upgradeDB.copyDatabase(dbName,
                                                    "other",
                                                    downgradeDB.getMongo().host,
                                                    jsTest.options().authUser,
                                                    jsTest.options().authPassword));
    } else {
        assert.commandWorked(upgradeDB.copyDatabase(dbName, "other", downgradeDB.getMongo().host));
    }
    assert.eq(0,
              upgradeConn.getDB("other").getCollectionInfos({name: "system.indexes"}).length,
              tojson(upgradeDB.getCollectionInfos()));

    MongoRunner.stopMongod(upgradeConn);
    MongoRunner.stopMongod(downgradeConn);
}());