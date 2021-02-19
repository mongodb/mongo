// Perform the upgrade/downgrade procedure by first setting the featureCompatibilityVersion and
// then switching the binary.
(function() {
"use strict";

load("jstests/replsets/rslib.js");
load("jstests/libs/get_index_helpers.js");
load("jstests/libs/check_uuids.js");
load("jstests/libs/check_unique_indexes.js");

const latestBinary = "latest";

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
                assert.commandWorked(
                    conn.getDB(dbs[j]).foo.insert(doc, {writeConcern: {w: "majority"}}));
            } else {
                assert.commandWorked(conn.getDB(dbs[j]).foo.insert(doc));
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
                assert.commandWorked(foo.createIndex({sno: 1}, {unique: true, v: 1}));
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
            if (c.type == "view") {
                return;
            }

            currentCollection.getIndexes().forEach(function(spec) {
                if (!spec.unique) {
                    return;
                }

                const ns = d.name + "." + c.name;
                if (spec.v === 1) {
                    unique_idx_v1.push({ns: ns, spec: spec});
                } else {
                    unique_idx.push({ns: ns, spec: spec});
                }
            });
        });
    });

    // Drop and create all v:2 indexes
    for (let pair of unique_idx) {
        const ns = pair.ns;
        const idx = pair.spec;
        let [dbName, collName] = ns.split(".");
        let res = db.getSiblingDB(dbName).runCommand({dropIndexes: collName, index: idx.name});
        assert.commandWorked(res);
        res = db.getSiblingDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{"key": idx.key, "name": idx.name, "unique": true}]
        });
        assert.commandWorked(res);
    }

    // Drop and create all v:1 indexes
    for (let pair of unique_idx_v1) {
        const ns = pair.ns;
        const idx = pair.spec;
        let [dbName, collName] = ns.split(".");
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
let standaloneTest = function(nodeOptions, downgradeVersion) {
    jsTestLog("Running standalone test with 'downgradeVersion': " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);
    let noCleanDataOptions = Object.assign({noCleanData: true}, nodeOptions);

    // New latest binary version standalone.
    jsTest.log("Starting a latest binVersion standalone");
    let conn = startMongodWithVersion(nodeOptions, latestBinary);
    let adminDB = conn.getDB("admin");

    // Insert some data.
    insertDataForConn(conn, ["admin", "local", "test"], nodeOptions);

    if (!nodeOptions.hasOwnProperty("shardsvr")) {
        // Initially featureCompatibilityVersion is latest except for when we run with shardsvr.
        // We expect featureCompatibilityVersion to be downgradeFCV for shardsvr.
        checkFCV(adminDB, latestFCV);

        // Ensure all collections have UUIDs and all unique indexes have new version in latest
        // featureCompatibilityVersion mode.
        checkCollectionUUIDs(adminDB);
        checkUniqueIndexFormatVersion(adminDB);

        setFCV(adminDB, downgradeFCV);
    } else {
        checkFCV(adminDB, lastLTSFCV);

        // Transitioning from last-lts to last-continuous is only allowed when
        // setFeatureCompatibilityVersion is called with fromConfigServer: true.
        assert.commandWorked(adminDB.runCommand(
            {setFeatureCompatibilityVersion: downgradeFCV, fromConfigServer: true}));
        checkFCV(adminDB, downgradeFCV);
    }

    // Ensure featureCompatibilityVersion is downgraded and all collections still have UUIDs.
    checkFCV(adminDB, downgradeFCV);
    checkCollectionUUIDs(adminDB);

    // Drop and recreate unique indexes with the older FCV
    recreateUniqueIndexes(adminDB, false);

    // Stop latest binary version mongod.
    MongoRunner.stopMongod(conn);

    // Start the downgraded binary version mongod with same dbpath
    jsTest.log("Starting a downgraded binVersion standalone to test downgrade");
    let downgradedConn = startMongodWithVersion(noCleanDataOptions, downgradeVersion);
    let downgradedAdminDB = downgradedConn.getDB("admin");

    // Check FCV document.
    checkFCV(downgradedAdminDB, downgradeFCV);

    // Ensure all collections still have UUIDs on a downgraded mongod.
    checkCollectionUUIDs(downgradedAdminDB);

    // Stop downgraded binary version mongod.
    MongoRunner.stopMongod(downgradedConn);

    // Start latest binary version mongod again.
    jsTest.log("Starting a latest binVersion standalone to test upgrade");
    conn = startMongodWithVersion(noCleanDataOptions, latestBinary);
    adminDB = conn.getDB("admin");

    // Ensure setFeatureCompatibilityVersion to latest succeeds, all collections have UUIDs
    // and all unique indexes are in new version.
    setFCV(adminDB, latestFCV);
    checkFCV(adminDB, latestFCV);
    checkCollectionUUIDs(adminDB);
    checkUniqueIndexFormatVersion(adminDB);

    // Stop latest binary version mongod for the last time
    MongoRunner.stopMongod(conn);
};

//
// Replica set tests.
//
let replicaSetTest = function(nodeOptions, downgradeVersion) {
    jsTestLog("Running replica set test with 'downgradeVersion': " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);
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
        // when we run with shardsvr. We expect featureCompatibilityVersion to be 'downgradeVersion'
        // for shardsvr.
        checkFCV(primaryAdminDB, latestFCV);

        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkFCV(secondaryAdminDB, latestFCV);
        }

        // Ensure all collections have UUIDs and unique indexes are in new version in latest
        // featureCompatibilityVersion mode on both primary and secondaries.
        checkCollectionUUIDs(primaryAdminDB);
        checkUniqueIndexFormatVersion(primaryAdminDB);
        for (let j = 0; j < secondaries.length; j++) {
            let secondaryAdminDB = secondaries[j].getDB("admin");
            checkCollectionUUIDs(secondaryAdminDB);
            checkUniqueIndexFormatVersion(secondaryAdminDB);
        }

        // Change featureCompatibilityVersion to downgradeFCV.
        setFCV(primaryAdminDB, downgradeFCV);
    } else {
        checkFCV(primaryAdminDB, lastLTSFCV);

        // Transitioning from last-lts to last-continuous is only allowed when
        // setFeatureCompatibilityVersion is called with fromConfigServer: true.
        assert.commandWorked(primaryAdminDB.runCommand(
            {setFeatureCompatibilityVersion: downgradeFCV, fromConfigServer: true}));
    }

    // Ensure featureCompatibilityVersion is 'downgradeVersion' and all collections still have
    // UUIDs.
    checkFCV(primaryAdminDB, downgradeFCV);
    // Ensure all replica set members have finished setting the FCV before checking secondaries.
    rst.awaitReplication();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        checkFCV(secondaryAdminDB, downgradeFCV);
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
    jsTest.log("Starting a " + downgradeVersion + " binVersion ReplSetTest to test downgrade");
    rst.startSet({restart: true, binVersion: downgradeVersion});

    // Check that the featureCompatiblityVersion is set to downgradeFCV and all
    // collections still have UUIDs.
    let downgradedPrimaryAdminDB = rst.getPrimary().getDB("admin");
    let downgradedSecondaries = rst.getSecondaries();

    checkFCV(downgradedPrimaryAdminDB, downgradeFCV);
    for (let j = 0; j < downgradedSecondaries.length; j++) {
        let secondaryAdminDB = downgradedSecondaries[j].getDB("admin");
        checkFCV(secondaryAdminDB, downgradeFCV);
    }

    checkCollectionUUIDs(downgradedPrimaryAdminDB);
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = downgradedSecondaries[j].getDB("admin");
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
    checkUniqueIndexFormatVersion(primaryAdminDB);
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryAdminDB = secondaries[j].getDB("admin");
        checkCollectionUUIDs(secondaryAdminDB);
        checkUniqueIndexFormatVersion(secondaryAdminDB);
    }

    rst.stopSet();
};

// Do tests for regular standalones and replica sets.
standaloneTest({}, 'last-continuous');
standaloneTest({}, 'last-lts');
replicaSetTest({}, 'last-continuous');
replicaSetTest({}, 'last-lts');

// Do tests for standalones and replica sets started with --shardsvr.
standaloneTest({shardsvr: ""}, 'last-continuous');
standaloneTest({shardsvr: ""}, 'last-lts');
replicaSetTest({shardsvr: ""}, 'last-continuous');
replicaSetTest({shardsvr: ""}, 'last-lts');

// Do tests for replica sets started with --configsvr.
replicaSetTest({configsvr: ""}, 'last-continuous');
replicaSetTest({configsvr: ""}, 'last-lts');
})();
