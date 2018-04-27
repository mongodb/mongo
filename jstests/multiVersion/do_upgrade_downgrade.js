// Perform the upgrade/downgrade procedure by first setting the featureCompatibilityVersion and
// then switching the binary.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/libs/get_index_helpers.js");
    load("jstests/libs/check_uuids.js");
    load("jstests/libs/check_unique_indexes.js");

    const latestBinary = "latest";
    const lastStableBinary = "last-stable";

    let setFCV = function(adminDB, version) {
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: version}));
        checkFCV(adminDB, version);
    };

    let insertDataForConn = function(conn, dbs, nodeOptions) {
        for (let i = 0; i < 20; i++) {
            let doc = {id: i, sno: i, a: "foo", conn: conn.name};
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

        // Create unique indexes on collection "foo" with two index formatVersions.
        // Providing index version explicitly allows index creation with corresponding
        // formatVersion.
        for (let j in dbs) {
            let testDB = conn.getDB(dbs[j]);
            testDB.getCollectionInfos().forEach(function(c) {
                if (c.name === "foo") {
                    let foo = testDB.getCollection(c.name);
                    assert.commandWorked(foo.createIndex({id: 1}, {unique: true}));
                    assert.commandWorked(
                        foo.createIndex({sno: 1}, {name: "sno_1"}, {unique: true, v: 1}));
                }
            });
        }
    };

    let recreateUniqueIndexes = function(db, secondary) {
        // Obtain list of all v1 and v2 unique indexes
        var unique_idx = [];
        var unique_idx_v1 = [];
        db.adminCommand("listDatabases").databases.forEach(function(d) {
            if (secondary && !(d.name === "local")) {
                // All replicated indexes will be dropped on the primary, and have that
                // drop propogated. Secondary nodes need to recreate unique indexes
                // associated with local collections.
                return;
            }
            let mdb = db.getSiblingDB(d.name);
            mdb.getCollectionInfos().forEach(function(c) {
                let currentCollection = mdb.getCollection(c.name);
                currentCollection.getIndexes().forEach(function(i) {
                    if (i.unique) {
                        if (i.v === "1") {
                            unique_idx_v1.push(i);
                            return;
                        }
                        unique_idx.push(i);
                    }
                });
            });
        });

        // Drop and create all v:2 indexes
        for (let idx of unique_idx) {
            let [dbName, collName] = idx.ns.split(".");
            let res = db.getSiblingDB(dbName).runCommand({dropIndexes: collName, index: idx.name});
            assert.commandWorked(res);
            res = db.getSiblingDB(dbName).runCommand({
                createIndexes: collName,
                indexes: [{"key": idx.key, "name": idx.name, "unique": true}]
            });
            assert.commandWorked(res);
        }

        // Drop and create all v:1 indexes
        for (let idx of unique_idx_v1) {
            let [dbName, collName] = idx.ns.split(".");
            let res = db.getSiblingDB(dbName).runCommand({dropIndexes: collName, index: idx.name});
            assert.commandWorked(res);
            res = db.getSiblingDB(dbName).runCommand({
                createIndexes: collName,
                indexes: [{"key": idx.key, "name": idx.name, "unique": true, "v": 1}]
            });
            assert.commandWorked(res);
        }
    };

    // Create and clear dbpath
    let sharedDbPath = MongoRunner.dataPath + "do_upgrade_downgrade";
    resetDbpath(sharedDbPath);

    // Return a mongodb connection with startup options, version and dbpath options
    let startMongodWithVersion = function(nodeOptions, ver, path) {
        let version = ver || latestBinary;
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

        // New latest binary version standalone.
        jsTest.log("Starting a latest binVersion standalone");
        let conn = startMongodWithVersion(nodeOptions, latestBinary);
        let adminDB = conn.getDB("admin");

        // Insert some data.
        insertDataForConn(conn, ["admin", "local", "test"], nodeOptions);

        if (!nodeOptions.hasOwnProperty("shardsvr")) {
            // Initially featureCompatibilityVersion is latest except for when we run with shardsvr.
            // We expect featureCompatibilityVersion to be last-stable for shardsvr.
            checkFCV(adminDB, latestFCV);

            // Ensure all collections have UUIDs and all unique indexes have new version in latest
            // featureCompatibilityVersion mode.
            checkCollectionUUIDs(adminDB);
            checkUniqueIndexFormatVersion(adminDB, latestFCV);

            // Set featureCompatibilityVersion to last-stable.
            setFCV(adminDB, lastStableFCV);
        }

        // Ensure featureCompatibilityVersion is last-stable and all collections still have UUIDs.
        checkFCV(adminDB, lastStableFCV);
        checkCollectionUUIDs(adminDB);

        // Drop and recreate unique indexes with the older FCV
        recreateUniqueIndexes(adminDB, false);

        // Stop latest binary version mongod.
        MongoRunner.stopMongod(conn);

        // Start last-stable binary version mongod with same dbpath
        jsTest.log("Starting a last-stable binVersion standalone to test downgrade");
        let lastStableConn = startMongodWithVersion(noCleanDataOptions, lastStableBinary);
        let lastStableAdminDB = lastStableConn.getDB("admin");

        // Check FCV document.
        checkFCV(lastStableAdminDB, lastStableFCV);

        // Ensure all collections still have UUIDs on a last-stable mongod.
        checkCollectionUUIDs(lastStableAdminDB);

        // Stop last-stable binary version mongod.
        MongoRunner.stopMongod(lastStableConn);

        // Start latest binary version mongod again.
        jsTest.log("Starting a latest binVersion standalone to test upgrade");
        conn = startMongodWithVersion(noCleanDataOptions, latestBinary);
        adminDB = conn.getDB("admin");

        // Ensure setFeatureCompatibilityVersion to latest succeeds, all collections have UUIDs
        // and all unique indexes are in new version.
        setFCV(adminDB, latestFCV);
        checkFCV(adminDB, latestFCV);
        checkCollectionUUIDs(adminDB);
        checkUniqueIndexFormatVersion(adminDB, latestFCV);

        // Stop latest binary version mongod for the last time
        MongoRunner.stopMongod(conn);
    };

    //
    // Replica set tests.
    //
    let replicaSetTest = function(nodeOptions) {

        // New latest binary version replica set.
        jsTest.log("Starting a latest binVersion ReplSetTest");
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
            // Initially featureCompatibilityVersion is latest on primary and secondaries except for
            // when we run with shardsvr. We expect featureCompatibilityVersion to be last-stable
            // for shardsvr.
            checkFCV(primaryAdminDB, latestFCV);

            for (let j = 0; j < secondaries.length; j++) {
                let secondaryAdminDB = secondaries[j].getDB("admin");
                checkFCV(secondaryAdminDB, latestFCV);
            }

            // Ensure all collections have UUIDs and unique indexes are in new version in latest
            // featureCompatibilityVersion mode on both primary and secondaries.
            checkCollectionUUIDs(primaryAdminDB);
            checkUniqueIndexFormatVersion(primaryAdminDB, latestFCV);
            for (let j = 0; j < secondaries.length; j++) {
                let secondaryAdminDB = secondaries[j].getDB("admin");
                checkCollectionUUIDs(secondaryAdminDB);
                checkUniqueIndexFormatVersion(secondaryAdminDB, latestFCV);
            }

            // Change featureCompatibilityVersion to last-stable.
            setFCV(primaryAdminDB, lastStableFCV);
            rst.awaitReplication();
        }

        // Ensure featureCompatibilityVersion is last-stable and all collections still have UUIDs.
        checkFCV(primaryAdminDB, lastStableFCV);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, lastStableFCV);
        }

        checkCollectionUUIDs(primaryAdminDB);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkCollectionUUIDs(secondaryAdminDB);
        }

        // Drop and recreate unique indexes with the older FCV
        recreateUniqueIndexes(primaryAdminDB, false);

        // Now drop and recreate unique indexes on secondaries' "local" database
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            recreateUniqueIndexes(secondaryAdminDB, true);
        }

        // Stop latest binary version replica set.
        rst.stopSet(null /* signal */, true /* forRestart */);

        // Downgrade the ReplSetTest binaries and make sure everything is okay.
        jsTest.log("Starting a last-stable binVersion ReplSetTest to test downgrade");
        rst.startSet({restart: true, binVersion: lastStableBinary});

        // Check that the featureCompatiblityVersion is set to last-stable and all
        // collections still have UUIDs.
        let lastStablePrimaryAdminDB = rst.getPrimary().getDB("admin");
        let lastStableSecondaries = rst.getSecondaries();

        checkFCV(lastStablePrimaryAdminDB, lastStableFCV);
        for (let j = 0; j < lastStableSecondaries.length; j++) {
            let secondaryAdminDB = lastStableSecondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, lastStableFCV);
        }

        checkCollectionUUIDs(lastStablePrimaryAdminDB);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = lastStableSecondaries[j].getDB("admin");
            checkCollectionUUIDs(secondaryAdminDB);
        }

        rst.stopSet(null /* signal */, true /* forRestart */);

        // Start latest binary version replica set again.
        jsTest.log("Starting a latest binVersion ReplSetTest to test upgrade");
        rst.startSet({restart: true, binVersion: latestBinary});
        primaryAdminDB = rst.getPrimary().getDB("admin");
        secondaries = rst.getSecondaries();

        // Ensure all collections have UUIDs and unique indexes are in new version after switching
        // back to latest featureCompatibilityVersion on both primary and secondaries.
        setFCV(primaryAdminDB, latestFCV);
        rst.awaitReplication();

        checkFCV(primaryAdminDB, latestFCV);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, latestFCV);
        }

        checkCollectionUUIDs(primaryAdminDB);
        checkUniqueIndexFormatVersion(primaryAdminDB, latestFCV);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkCollectionUUIDs(secondaryAdminDB);
            checkUniqueIndexFormatVersion(secondaryAdminDB, latestFCV);
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
