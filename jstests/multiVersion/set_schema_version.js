// Tests for setFeatureCompatibilityVersion.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/get_index_helpers.js");

    const latest = "latest";
    const downgrade = "3.4";

    let checkCollectionUUIDs = function(adminDB, isDowngrade, excludeSysIndexes) {
        let databaseList = adminDB.runCommand({"listDatabases": 1}).databases;

        databaseList.forEach(function(database) {
            let currentDatabase = adminDB.getSiblingDB(database.name);
            let collectionInfos = currentDatabase.getCollectionInfos();
            for (let i = 0; i < collectionInfos.length; i++) {
                if (excludeSysIndexes) {
                    // Exclude system.indexes and all collections in local until SERVER-29926
                    // and SERVER-30131 are fixed.
                    if (collectionInfos[i].name != "system.indexes" && currentDatabase != "local") {
                        assert(collectionInfos[i].info.uuid);
                    }
                } else {
                    if (isDowngrade) {
                        assert(!collectionInfos[i].info.uuid);
                    } else {
                        assert(collectionInfos[i].info.uuid);
                    }
                }
            }
        });
    };

    //
    // Standalone tests.
    //

    let dbpath = MongoRunner.dataPath + "feature_compatibility_version";
    resetDbpath(dbpath);

    // New 3.6 standalone.
    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    let adminDB = conn.getDB("admin");

    // Initially featureCompatibilityVersion is 3.6.
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version, "3.6");

    // Ensure all collections have UUIDs in 3.6 mode.
    checkCollectionUUIDs(adminDB, false, true);

    // Set featureCompatibilityVersion to 3.4.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");

    // Ensure no collections in a featureCompatibilityVersion 3.4 database have UUIDs.
    adminDB = conn.getDB("admin");
    checkCollectionUUIDs(adminDB, true, false);

    // Ensure all collections have UUIDs after switching back to featureCompatibilityVersion 3.6.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    checkCollectionUUIDs(adminDB, false, false);

    //
    // Replica set tests.
    //

    // New 3.6 replica set.
    let rst = new ReplSetTest({nodes: 3, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    let primaryAdminDB = rst.getPrimary().getDB("admin");
    let secondaries = rst.getSecondaries();

    // Initially featureCompatibilityVersion is 3.6 on primary and secondaries.
    res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.6");
    rst.awaitReplication();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(res);
        assert.eq(res.featureCompatibilityVersion, "3.6");
        assert.eq(
            secondaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
            "3.6");
    }

    // Ensure all collections have UUIDs in 3.6 mode on both primary and secondaries.
    checkCollectionUUIDs(primaryAdminDB, false, true);
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        checkCollectionUUIDs(secondaryAdminDB, false, true);
    }

    // Change featureCompatibilityVersion to 3.4.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.4");
    assert.eq(primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.4");
    rst.awaitReplication();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(res);
        assert.eq(res.featureCompatibilityVersion, "3.4");
        assert.eq(
            secondaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
            "3.4");
    }

    // Ensure no collections have UUIDs in 3.4 mode on both primary and secondaries.
    checkCollectionUUIDs(primaryAdminDB, true, false);
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        checkCollectionUUIDs(secondaryAdminDB, true, false);
    }

    // Ensure all collections have UUIDs after switching back to featureCompatibilityVersion 3.6 on
    // both primary and secondaries.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    res = primaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq(res.featureCompatibilityVersion, "3.6");
    assert.eq(primaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
              "3.6");
    rst.awaitReplication();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        res = secondaryAdminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(res);
        assert.eq(res.featureCompatibilityVersion, "3.6");
        assert.eq(
            secondaryAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
            "3.6");
    }

    checkCollectionUUIDs(primaryAdminDB, false, false);
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        checkCollectionUUIDs(secondaryAdminDB, false, false);
    }

    rst.stopSet();
})();
