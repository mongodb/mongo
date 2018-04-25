/**
 * This test verifies that if mongod exits after setting the FCV document to 3.4 before the
 * collection UUIDs are removed, that re-running the downgrade to 3.4 is possible.
 */
(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    let setFCV = function(adminDB, version) {
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: version}));
    };

    // Inserts test data
    let insertDataForConn = function(conn, dbs) {
        for (let i = 0; i < 20; i++) {
            let doc = {id: i, a: "foo", conn: conn.name};
            for (let j in dbs) {
                assert.writeOK(conn.getDB(dbs[j]).foo.insert(doc));
            }
        }
    };

    // Verifies that UUIDs exist when uuidsExpected is true, and that UUIDs do not exist when
    // uuidsExpected is false.
    let checkCollectionUUIDs = function(adminDB, uuidsExpected, excludeLocal = true) {
        let databaseList = adminDB.runCommand({"listDatabases": 1}).databases;

        databaseList.forEach(function(database) {
            if (excludeLocal && database.name == "local") {
                return;
            }
            let currentDatabase = adminDB.getSiblingDB(database.name);
            let collectionInfos = currentDatabase.getCollectionInfos();
            for (let i = 0; i < collectionInfos.length; i++) {
                // Always skip system.indexes due to SERVER-30500.
                if (collectionInfos[i].name == "system.indexes") {
                    continue;
                }
                // Exclude checking all collections in local until SERVER-30131 is fixed.
                if (!uuidsExpected) {
                    assert(!collectionInfos[i].info.uuid,
                           "Unexpected uuid for collection: " + tojson(collectionInfos[i]));
                } else {
                    assert(collectionInfos[i].info.uuid,
                           "Expect uuid for collection: " + tojson(collectionInfos[i]));
                }
            }
        });
    };

    jsTest.log("Testing Incomplete Downgrade");

    let dbpath = MongoRunner.dataPath + "incomplete_downgrade";
    resetDbpath(dbpath);

    let conn = MongoRunner.runMongod({dbpath: dbpath});
    assert.neq(null, conn, "mongod was unable to start up");
    let adminDB = conn.getDB("admin");

    // Insert test data.
    insertDataForConn(conn, ["admin", "local", "test"]);

    // Ensure featureCompatibilityVersion is 3.6 and collections have UUIDs.
    checkFCV(adminDB, "3.6");
    checkCollectionUUIDs(adminDB, true);

    // Set failure point to exit after setting featureCompatibilityVersion document before removing
    // UUIDs from collections.
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "featureCompatibilityDowngrade", mode: "alwaysOn"}));

    // Downgrade, which should trigger a shutdown, so don't check for result.
    // Mongod should stop responding.
    assert.soon(function() {
        try {
            adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"});
        } catch (e) {
            return true;
        }
    }, "Mongod should stop responding", 10 * 1000);

    // Check for clean exit.
    MongoRunner.validateCollectionsCallback = function() {};
    MongoRunner.stopMongod(conn);

    clearRawMongoProgramOutput();

    // Start mongod again with the same dbpath
    conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");
    adminDB = conn.getDB("admin");

    // FeatureCompatibility version should be 3.4 and targetVersion should be 3.4.
    checkFCV(adminDB, "3.4", "3.4");

    // Verify startup warnings
    let msgUpgrade = "WARNING: A featureCompatibilityVersion upgrade did not complete";
    let msgDowngrade = "WARNING: A featureCompatibilityVersion downgrade did not complete";
    let msg2 = "use the setFeatureCompatibilityVersion command to resume";
    assert.soon(function() {
        return rawMongoProgramOutput().match(msgDowngrade) && rawMongoProgramOutput().match(msg2);
    }, "Mongod should have printed startup warning about collections having UUIDs in FCV 3.4");

    // Check that collections still have UUIDs.
    checkCollectionUUIDs(adminDB, /* uuidsExpected */ true);

    // Now that failpoint is not set, resume downgrade.
    setFCV(adminDB, "3.4");

    // Verify no collections have UUIDS now.
    checkCollectionUUIDs(adminDB, false);

    jsTest.log("Testing Incomplete Upgrade");

    // Set failure point to exit before setting featureCompatibilityVersion document and after
    // creating UUIDs for collections.
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "featureCompatibilityUpgrade", mode: "alwaysOn"}));

    // Try upgrade to 3.6, which triggers the failpoint to exit, immediately after creating UUIDs.
    assert.soon(function() {
        try {
            adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"});
        } catch (e) {
            return true;
        }
    }, "Mongod should stop responding", 10 * 1000);

    // Verify mongod stopped cleanly
    MongoRunner.stopMongod(conn);

    clearRawMongoProgramOutput();

    // Start mongod again with the same dbpath
    conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
    assert.neq(null, conn, "mongod was unable to start up");
    adminDB = conn.getDB("admin");

    // Verify startup warnings
    assert.soon(function() {
        return rawMongoProgramOutput().match(msgUpgrade) && rawMongoProgramOutput().match(msg2);
    }, "Mongod should have printed startup warning about collections having UUIDs in FCV 3.4");

    // Check that collections have UUIDs.
    checkCollectionUUIDs(adminDB, /* uuidsExpected */ true);

    // The FCV document should not be updated yet, but the targetVersion should be 3.6
    checkFCV(adminDB, "3.4", "3.6");

    // Now that failpoint is not set, resume upgrade.
    setFCV(adminDB, "3.6");

    // Verify all collections still have UUIDS.
    checkCollectionUUIDs(adminDB, true);

    // Verify FCV.
    checkFCV(adminDB, "3.6");

    MongoRunner.stopMongod(conn);

})();
