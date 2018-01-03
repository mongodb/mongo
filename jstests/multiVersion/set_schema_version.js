// Tests for setFeatureCompatibilityVersion.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/get_index_helpers.js");
    load("jstests/libs/check_uuids.js");

    const latest = "latest";
    const downgrade = "3.4";

    let setFCV = function(adminDB, version) {
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: version}));
        checkFCV(adminDB, version);
    };

    let insertDataForConn = function(conn, dbs, nodeOptions) {
        for (let i = 0; i < 20; i++) {
            let doc = {id: i, a: "foo", conn: conn.name};
            for (let j in dbs) {
                if (nodeOptions.hasOwnProperty("configsvr")) {
                    if (j !== "admin" && j !== "local") {
                        // We can't create user databases on a --configsvr instance.
                        continue;
                    }
                    // Config servers have a majority write concern.
                    assert.writeOK(
                        conn.getDB(dbs[j]).foo.insert(doc, {writeConcern: {w: "majority"}}));
                } else {
                    assert.writeOK(conn.getDB(dbs[j]).foo.insert(doc));
                }
            }
        }
    };

    // Create and clear dbpath
    let sharedDbPath = MongoRunner.dataPath + "set_schema_version";
    resetDbpath(sharedDbPath);

    // Return a mongodb connection with startup options, version and dbpath options
    let startMongodWithVersion = function(nodeOptions, ver, path) {
        let version = ver || latest;
        let dbpath = path || sharedDbPath;
        let conn = MongoRunner.runMongod(
            Object.assign({}, nodeOptions, {dbpath: dbpath, binVersion: version}));
        assert.neq(null,
                   conn,
                   "mongod was unable to start up with version=" + version + " and path=" + dbpath);
        return conn;
    };

    //
    // Standalone tests.
    //
    let standaloneTest = function(nodeOptions) {
        let noCleanDataOptions = Object.assign({noCleanData: true}, nodeOptions);

        // New 3.6 standalone.
        jsTest.log("Starting a binVersion 3.6 standalone");
        let conn = startMongodWithVersion(nodeOptions, latest);
        let adminDB = conn.getDB("admin");

        // Insert some data.
        insertDataForConn(conn, ["admin", "local", "test"], nodeOptions);

        if (!nodeOptions.hasOwnProperty("shardsvr")) {
            // Initially featureCompatibilityVersion is 3.6 except for when we run with shardsvr.
            // We expect featureCompatibilityVersion to be 3.4 for shardsvr.
            checkFCV(adminDB, "3.6");

            // Ensure all collections have UUIDs in 3.6 mode.
            checkCollectionUUIDs(adminDB, false);

            // Set featureCompatibilityVersion to 3.4.
            setFCV(adminDB, "3.4");
        }

        // Ensure featureCompatibilityVersion is 3.4.
        checkFCV(adminDB, "3.4");

        // Ensure no collections in a featureCompatibilityVersion 3.4 database have UUIDs.
        checkCollectionUUIDs(adminDB, true);

        // Stop Mongod 3.6
        MongoRunner.stopMongod(conn);

        // Start Mongod 3.4 with same dbpath
        jsTest.log("Starting a binVersion 3.4 standalone to test downgrade");
        let downgradeConn = startMongodWithVersion(noCleanDataOptions, downgrade);
        let downgradeAdminDB = downgradeConn.getDB("admin");

        // Check FCV document
        checkFCV34(downgradeAdminDB, "3.4");

        // Ensure there are no UUIDs
        checkCollectionUUIDs(downgradeAdminDB, true);

        // Stop 3.4
        MongoRunner.stopMongod(downgradeConn);

        // Start 3.6 again
        jsTest.log("Starting a binVersion 3.6 standalone to test upgrade");
        conn = startMongodWithVersion(noCleanDataOptions, latest);
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
    let replicaSetTest = function(nodeOptions) {

        // New 3.6 replica set.
        jsTest.log("Starting a binVersion 3.6 ReplSetTest");
        let rst = new ReplSetTest({nodes: 3, nodeOptions: nodeOptions});
        rst.startSet();
        rst.initiate();
        let primaryAdminDB = rst.getPrimary().getDB("admin");
        let secondaries = rst.getSecondaries();

        // Insert some data.
        insertDataForConn(rst.getPrimary(), ["admin", "local", "test"], nodeOptions);
        rst.awaitReplication();

        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            // Insert some data into the local DB.
            insertDataForConn(secondaries[j], ["local"], nodeOptions);
        }

        if (!nodeOptions.hasOwnProperty("shardsvr")) {
            // Initially featureCompatibilityVersion is 3.6 on primary and secondaries except for
            // when we run with shardsvr. We expect featureCompatibilityVersion to be 3.4 for
            // shardsvr.
            checkFCV(primaryAdminDB, "3.6");

            for (let j = 0; j < secondaries.length; j++) {
                let secondaryAdminDB = secondaries[j].getDB("admin");
                checkFCV(secondaryAdminDB, "3.6");
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
        }

        // Ensure featureCompatibilityVersion is 3.4.
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
        rst.stopSet(null /* signal */, true /* forRestart */);

        // Downgrade RS the 3.4 binary, and make sure everything is okay
        jsTest.log("Starting a binVersion 3.4 ReplSetTest to test downgrade");
        rst.startSet({restart: true, binVersion: downgrade});

        // Check that the featureCompatiblityDocument is set to 3.4 and that there are no UUIDs
        let downgradePrimaryAdminDB = rst.getPrimary().getDB("admin");
        let downgradeSecondaries = rst.getSecondaries();

        // Initially featureCompatibilityVersion document is 3.4 on primary and secondaries.
        checkCollectionUUIDs(downgradePrimaryAdminDB, true);
        checkFCV34(downgradePrimaryAdminDB, "3.4");
        for (let j = 0; j < downgradeSecondaries.length; j++) {
            let secondaryAdminDB = downgradeSecondaries[j].getDB("admin");
            checkFCV34(secondaryAdminDB, "3.4");

            // Ensure no collections have UUIDs
            checkCollectionUUIDs(secondaryAdminDB, true);
        }

        rst.stopSet(null /* signal */, true /* forRestart */);

        // Start 3.6 replica set again
        jsTest.log("Starting a binVersion 3.6 ReplSetTest to test upgrade");
        rst.startSet({restart: true, binVersion: latest});
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

    // Do tests for regular standalones and replica sets.
    standaloneTest({});
    replicaSetTest({});

    // Do tests for standalones and replica sets started with --shardsvr.
    standaloneTest({shardsvr: ""});
    replicaSetTest({shardsvr: ""});

    // Do tests for standalones and replica sets started with --configsvr.
    if (jsTest.options().storageEngine !== "mmapv1") {
        // We don't allow starting config servers with the MMAP storage engine.
        standaloneTest({configsvr: ""});
        replicaSetTest({configsvr: ""});
    }
})();
