/**
 * Tests --repair correctly restores a missing feature compatibility version document on startup,
 * and that regular startup without --repair fails if the FCV document is missing.
 */

(function() {
"use strict";

load("jstests/libs/feature_compatibility_version.js");

let dbpath = MongoRunner.dataPath + "feature_compatibility_version";
resetDbpath(dbpath);
let connection;
let adminDB;

const latest = "latest";

/**
 * Ensure that a mongod (without using --repair) fails to start up if there are non-local
 * collections and the FCV document in the admin database has been removed.
 *
 * The mongod has 'version' binary and is started up on 'dbpath'.
 */
let doStartupFailTests = function(version, dbpath) {
    // Set up a mongod with an admin database but without a FCV document in the admin database.
    setupMissingFCVDoc(version, dbpath);

    // Now attempt to start up a new mongod without clearing the data files from 'dbpath', which
    // contain the admin database but are missing the FCV document. The mongod should fail to
    // start up if there is a non-local collection and the FCV document is missing.
    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: version, noCleanData: true});
    assert.eq(null,
              conn,
              "expected mongod to fail when data files are present but no FCV document is found.");
};

/**
 * Starts up a mongod with binary 'version' on 'dbpath', then removes the FCV document from the
 * admin database and returns the mongod.
 */
let setupMissingFCVDoc = function(version, dbpath) {
    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: version});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + version + " and no data files");
    adminDB = conn.getDB("admin");
    removeFCVDocument(adminDB);
    MongoRunner.stopMongod(conn);
    return conn;
};

// Check that start up without --repair fails if there is non-local DB data and the FCV doc was
// deleted.
doStartupFailTests(latest, dbpath);

// --repair can be used to restore a missing featureCompatibilityVersion document to an existing
// admin database, as long as all collections have UUIDs. The FCV should be initialized to
// lastStableFCV / downgraded FCV.
connection = setupMissingFCVDoc(latest, dbpath);
let returnCode =
    runMongoProgram("mongod", "--port", connection.port, "--repair", "--dbpath", dbpath);
assert.eq(
    returnCode,
    0,
    "expected mongod --repair to execute successfully when restoring a missing FCV document.");

connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
assert.neq(null,
           connection,
           "mongod was unable to start up with version=" + latest + " and existing data files");
adminDB = connection.getDB("admin");
assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).version,
          lastStableFCV);
assert.eq(adminDB.system.version.findOne({_id: "featureCompatibilityVersion"}).targetVersion, null);
MongoRunner.stopMongod(connection);

// If the featureCompatibilityVersion document is present, --repair should just return success.
connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
assert.neq(null,
           connection,
           "mongod was unable to start up with version=" + latest + " and no data files");
MongoRunner.stopMongod(connection);

returnCode = runMongoProgram("mongod", "--port", connection.port, "--repair", "--dbpath", dbpath);
assert.eq(returnCode, 0);
})();
