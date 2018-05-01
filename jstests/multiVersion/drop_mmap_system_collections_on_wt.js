// Ensure system.indexes created on wiredTiger 3.4 does not cause mongodb 4.0 to fail to start up
// (see SERVER-34482). This test is to be removed after mongodb 4.0.
(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/check_uuids.js");
    load("jstests/multiVersion/libs/multi_rs.js");

    const UUIDCheckBinary = "latest";
    const UUIDBinary = "3.6";
    const noUUIDBinary = "3.4";
    const systemIndexesDB = "systemIndexesDB";
    const UUIDFCV = "3.6";

    // This test is irrelevant for MMAP.
    if (jsTest.options().storageEngine == "mmapv1") {
        return;
    }

    // Create system.indexes on wiredTiger, using a version of mongod that does not assign UUIDs.
    let rst = new ReplSetTest({
        nodes: 3,
        nodeOptions: {binVersion: noUUIDBinary},
    });
    rst.startSet();
    rst.initiate();
    let primaryTestDB = rst.getPrimary().getDB(systemIndexesDB);
    primaryTestDB.dropDatabase();
    const systemIndexesColl = "system.indexes";
    const createCmd = {create: systemIndexesColl, writeConcern: {w: "majority"}};
    assert.commandWorked(primaryTestDB.runCommand(createCmd),
                         "expected " + tojson(createCmd) + " to succeed");
    rst.awaitReplication();
    assert(primaryTestDB.getCollectionInfos().length == 1);
    assert(primaryTestDB.getCollectionInfos()[0].name == systemIndexesColl);
    let secondaries = rst.getSecondaries();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryTestDB = secondaries[j].getDB(systemIndexesDB);
        assert(secondaryTestDB.getCollectionInfos().length == 1);
        assert(secondaryTestDB.getCollectionInfos()[0].name == systemIndexesColl);
    }

    // Restart with mongodb 3.6 and set the FCV to 3.6, which will assign UUIDs to all
    // collections except for system.indexes and system.namespaces.
    rst.upgradeSet({binVersion: UUIDBinary});

    let primaryAdminDB = rst.getPrimary().getDB("admin");
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: UUIDFCV}));
    rst.awaitReplication();
    checkFCV(primaryAdminDB, UUIDFCV);
    secondaries = rst.getSecondaries();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        checkFCV(secondaryAdminDB, UUIDFCV);
    }

    primaryTestDB = rst.getPrimary().getDB(systemIndexesDB);
    assert(!primaryTestDB.getCollectionInfos()[0].info.uuid);

    // Restart with mongodb 4.0. This will drop system.indexes and system.namespaces if they
    // exist, allowing the UUID check on start up to pass.
    rst.upgradeSet({binVersion: UUIDCheckBinary});

    primaryTestDB = rst.getPrimary().getDB(systemIndexesDB);
    assert(primaryTestDB.getCollectionInfos().length == 0);

    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaries = rst.getSecondaries();
    checkCollectionUUIDs(primaryAdminDB);
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryTestDB = secondaries[j].getDB(systemIndexesDB);
        assert(secondaryTestDB.getCollectionInfos().length == 0);
        let secondaryAdminDB = secondaries[j].getDB("admin");
        checkCollectionUUIDs(secondaryAdminDB);
    }
    rst.stopSet();
})();
