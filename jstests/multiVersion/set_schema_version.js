// Tests for setFeatureCompatibilityVersion.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/get_index_helpers.js");

    const latest = "latest";
    const downgrade = "3.4";

    let checkCollectionUUIDs = function(adminDB, isDowngrade) {
        let databaseList = adminDB.runCommand({"listDatabases": 1}).databases;

        databaseList.forEach(function(database) {
            let currentDatabase = adminDB.getSiblingDB(database.name);
            let collectionInfos = currentDatabase.getCollectionInfos();
            for (let i = 0; i < collectionInfos.length; i++) {
                // Always skip system.indexes due to SERVER-30500.
                if (collectionInfos[i].name == "system.indexes") {
                    continue;
                }
                if (isDowngrade) {
                    assert(!collectionInfos[i].info.uuid,
                           "Unexpected uuid for collection: " + tojson(collectionInfos[i]));
                } else {
                    assert(collectionInfos[i].info.uuid,
                           "Expect uuid for collection: " + tojson(collectionInfos[i]));
                }
            }
        });
    };

    let checkFCV = function(adminDB, version, is34) {
        if (!is34) {
            let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
            assert.commandWorked(res);
            assert.eq(res.featureCompatibilityVersion, version);
        }
        assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
                  version);
    };

    let setFCV = function(adminDB, version) {
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: version}));
        checkFCV(adminDB, version);
    };

    let insertDataForConn = function(conn, dbs) {
        for (let i = 0; i < 20; i++) {
            let doc = {id: i, a: "foo", conn: conn.name};
            for (let j in dbs) {
                assert.writeOK(conn.getDB(dbs[j]).foo.insert(doc));
            }
        }
    };

    // Create and clear dbpath
    let sharedDbPath = MongoRunner.dataPath + "set_schema_version";
    resetDbpath(sharedDbPath);

    // Return a mongodb connection with version and dbpath options
    let startMongodWithVersion = function(ver, path) {
        let version = ver || latest;
        let dbpath = path || sharedDbPath;
        let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: version});
        assert.neq(null,
                   conn,
                   "mongod was unable to start up with version=" + version + " and path=" + dbpath);
        return conn;
    };

    //
    // Standalone tests.
    //
    let standaloneTest = function() {

        // New 3.6 standalone.
        let conn = startMongodWithVersion(latest);
        let adminDB = conn.getDB("admin");

        // Initially featureCompatibilityVersion is 3.6.
        checkFCV(adminDB, "3.6");

        // Insert some data
        insertDataForConn(conn, ["admin", "local", "test"]);

        // Ensure all collections have UUIDs in 3.6 mode.
        checkCollectionUUIDs(adminDB, false);

        // Set featureCompatibilityVersion to 3.4.
        setFCV(adminDB, "3.4");

        // Ensure no collections in a featureCompatibilityVersion 3.4 database have UUIDs.
        checkCollectionUUIDs(adminDB, true);

        // Stop Mongod 3.6
        MongoRunner.stopMongod(conn);

        // Start Mongod 3.4 with same dbpath
        jsTest.log("Starting MongoDB 3.4 to test downgrade");
        let downgradeConn = startMongodWithVersion(downgrade);
        let downgradeAdminDB = downgradeConn.getDB("admin");

        // Check FCV document
        checkFCV(downgradeAdminDB, "3.4", true);

        // Ensure there are no UUIDs
        checkCollectionUUIDs(downgradeAdminDB, true);

        // Stop 3.4
        MongoRunner.stopMongod(downgradeConn);

        // Start 3.6 again
        jsTest.log("Starting MongoDB 3.6 to test upgrade");
        conn = startMongodWithVersion(latest);
        adminDB = conn.getDB("admin");

        // Ensure all collections have UUIDs after switching back to featureCompatibilityVersion
        // 3.6.
        setFCV(adminDB, "3.6");
        checkCollectionUUIDs(adminDB, false);

        // Stop Mongod 3.6 for the last time
        MongoRunner.stopMongod(conn);
    };

    //
    // Replica set tests.
    //
    let replicaSetTest = function() {

        // New 3.6 replica set.
        let rst = new ReplSetTest({nodes: 3, binVersion: latest});
        rst.startSet();
        rst.initiate();
        let primaryAdminDB = rst.getPrimary().getDB("admin");
        let secondaries = rst.getSecondaries();

        // Initially featureCompatibilityVersion is 3.6 on primary and secondaries, and insert some
        // data.
        checkFCV(primaryAdminDB, "3.6");
        insertDataForConn(rst.getPrimary(), ["admin", "local", "test"]);
        rst.awaitReplication();

        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, "3.6");
            // Insert some data into the local DB
            insertDataForConn(secondaries[j], ["local"]);
        }

        // Ensure all collections have UUIDs in 3.6 mode on both primary and secondaries.
        checkCollectionUUIDs(primaryAdminDB, false);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkCollectionUUIDs(secondaryAdminDB, false);
        }

        // Change featureCompatibilityVersion to 3.4.
        setFCV(primaryAdminDB, "3.4");
        rst.awaitReplication();
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, "3.4");
        }

        // Ensure no collections have UUIDs in 3.4 mode on both primary and secondaries.
        checkCollectionUUIDs(primaryAdminDB, true);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkCollectionUUIDs(secondaryAdminDB, true);
        }

        // Stop 3.6 RS
        rst.stopSet();

        // Downgrade RS the 3.4 binary, and make sure everything is okay
        let downgradeRst = new ReplSetTest({nodes: 3, nodeOptions: {binVersion: downgrade}});
        downgradeRst.startSet();
        downgradeRst.initiate();

        // Check that the featureCompatiblityDocument is set to 3.4 and that there are no UUIDs
        let downgradePrimaryAdminDB = downgradeRst.getPrimary().getDB("admin");
        let downgradeSecondaries = downgradeRst.getSecondaries();

        // Initially featureCompatibilityVersion document is 3.4 on primary and secondaries.
        checkCollectionUUIDs(downgradePrimaryAdminDB, true);
        checkFCV(downgradePrimaryAdminDB, "3.4", true);
        for (let j = 0; j < downgradeSecondaries.length; j++) {
            let secondaryAdminDB = downgradeSecondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, "3.4", true);

            // Ensure no collections have UUIDs
            checkCollectionUUIDs(secondaryAdminDB, true);
        }

        downgradeRst.stopSet();

        // Start 3.6 cluster again
        rst = new ReplSetTest({nodes: 3, binVersion: latest});
        rst.startSet();
        rst.initiate();
        primaryAdminDB = rst.getPrimary().getDB("admin");
        secondaries = rst.getSecondaries();

        // Ensure all collections have UUIDs after switching back to featureCompatibilityVersion 3.6
        // on both primary and secondaries.
        setFCV(primaryAdminDB, "3.6");
        rst.awaitReplication();
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, "3.6");
        }

        checkCollectionUUIDs(primaryAdminDB, false);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkCollectionUUIDs(secondaryAdminDB, false);
        }

        rst.stopSet();
    };

    // Do tests
    standaloneTest();
    replicaSetTest();

})();
