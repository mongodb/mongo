/**
 * Tests that a v4.0 mongod cannot start up if any collection is missing a UUID.
 *
 * Utilizes a v3.6 binary and downgrade to FCV 3.4 to set up collections without UUIDs
 *
 * Also demonstrate a v4.0 mongod will shutdown to 3.4 compatible data files when started up on
 * 3.4 data files.
 */

(function() {
    "use strict";

    let dbpath = MongoRunner.dataPath + "startup_without_UUIDs";
    resetDbpath(dbpath);
    let connection;

    connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4"});
    assert.commandWorked(connection.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    MongoRunner.stopMongod(connection);

    jsTest.log("Asserting a v4.0 binary does not startup on 3.4 data files.");

    let returnCode = runMongoProgram("mongod", "--port", connection.port, "--dbpath", dbpath, "-v");
    const needsUpgradeCode = 62;
    assert.eq(returnCode, needsUpgradeCode);

    jsTest.log("Asserting v3.4 does start up after a v4.0 binary failed.");

    connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4"});
    MongoRunner.stopMongod(connection);

    jsTest.log("Set up a v3.6 binary downgraded to FCV 3.4 without collection UUIDs.");

    connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.6"});
    assert.commandWorked(connection.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
    MongoRunner.stopMongod(connection);

    jsTest.log("Attempting v4.0 --repair should fail because collections lack UUIDs.");

    returnCode =
        runMongoProgram("mongod", "--port", connection.port, "--repair", "--dbpath", dbpath);
    assert.neq(returnCode, 0);

    jsTest.log("Trying to start up a v4.0 binary on FCV 3.4 data files should fail because the " +
               "collections are missing UUIDs.");

    connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.eq(null, connection);

    jsTest.log("Using a v3.6 binary and upgrading to FCV 3.6 should update the data files to a " +
               "format acceptable to a v4.0 binary");

    connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.6", noCleanData: true});
    assert.neq(null, connection);
    assert.commandWorked(connection.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    MongoRunner.stopMongod(connection);

    jsTest.log("Now, finally, a v4.0 binary should start up on the FCV 3.6 data files");

    connection = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
    assert.neq(null, connection);
    MongoRunner.stopMongod(connection);

    jsTest.log("Done!");

})();
