/**
 * Test that mongod will not allow creation of collection validators using 3.6 query features when
 * the feature compatibility version is older than 3.6.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    const testName = "collection_validator_feature_compatibility_version";
    const dbpath = MongoRunner.dataPath + testName;

    /**
     * Tests the correct behavior of a collection validator using 3.6 query features with different
     * binary versions and feature compatibility versions. 'validator' should be a collection
     * validator using a new 3.6 query feature, and 'nonMatchingDocument' should be a document that
     * does not match 'validator'.
     */
    function testValidator(validator, nonMatchingDocument) {
        resetDbpath(dbpath);

        let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
        assert.neq(null, conn, "mongod was unable to start up");

        let testDB = conn.getDB(testName);

        let adminDB = conn.getDB("admin");

        // Explicitly set feature compatibility version 3.6.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

        // Create a collection with a validator using 3.6 query features.
        assert.commandWorked(testDB.createCollection("coll", {validator: validator}));
        let coll = testDB.coll;

        // The validator should cause this insert to fail.
        assert.writeError(coll.insert(nonMatchingDocument), ErrorCodes.DocumentValidationFailure);

        // Set a validator using 3.6 query features on an existing collection.
        coll.drop();
        assert.commandWorked(testDB.createCollection("coll"));
        assert.commandWorked(testDB.runCommand({collMod: "coll", validator: validator}));

        // Another failing update.
        assert.writeError(coll.insert(nonMatchingDocument), ErrorCodes.DocumentValidationFailure);

        // Set the feature compatibility version to 3.4.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));

        // The validator is already in place, so it should still cause this insert to fail.
        assert.writeError(coll.insert(nonMatchingDocument), ErrorCodes.DocumentValidationFailure);

        // Trying to create a new collection with a validator using 3.6 query features should fail
        // while feature compatibility version is 3.4.
        let res = testDB.createCollection("coll2", {validator: validator});
        assert.commandFailedWithCode(res, ErrorCodes.QueryFeatureNotAllowed);
        assert(res.errmsg.match(/featureCompatibilityVersion/),
               "Expected error message from createCollection referencing " +
                   "'featureCompatibilityVersion' but instead got: " + res.errmsg);

        // Trying to update a collection with a validator using 3.6 query features should also fail.
        res = testDB.runCommand({collMod: "coll", validator: validator});
        assert.commandFailedWithCode(res, ErrorCodes.QueryFeatureNotAllowed);
        assert(
            res.errmsg.match(/featureCompatibilityVersion/),
            "Expected error message from collMod referencing 'featureCompatibilityVersion' but " +
                "instead got: " + res.errmsg);

        MongoRunner.stopMongod(conn);

        // If we try to start up a 3.4 mongod, it will fail, because it will not be able to parse
        // the validator using 3.6 query features.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4", noCleanData: true});
        assert.eq(null,
                  conn,
                  "mongod 3.4 started, even with a validator using 3.6 query features in place.");

        // Starting up a 3.6 mongod, however, should succeed, even though the feature compatibility
        // version is still set to 3.4.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
        assert.neq(null, conn, "mongod was unable to start up");

        adminDB = conn.getDB("admin");
        testDB = conn.getDB(testName);
        coll = testDB.coll;

        // And the validator should still work.
        assert.writeError(coll.insert(nonMatchingDocument), ErrorCodes.DocumentValidationFailure);

        // Remove the validator.
        assert.commandWorked(testDB.runCommand({collMod: "coll", validator: {}}));

        MongoRunner.stopMongod(conn);

        // Now, we should be able to start up a 3.4 mongod.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4", noCleanData: true});
        assert.neq(
            null,
            conn,
            "mongod 3.4 failed to start, even after we removed the validator using 3.6 query features");

        MongoRunner.stopMongod(conn);

        // The rest of the test uses mongod 3.6.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
        assert.neq(null, conn, "mongod was unable to start up");

        adminDB = conn.getDB("admin");
        testDB = conn.getDB(testName);
        coll = testDB.coll;

        // Set the feature compatibility version back to 3.6.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

        // Now we should be able to create a collection with a validator using 3.6 query features
        // again.
        assert.commandWorked(testDB.createCollection("coll2", {validator: validator}));

        // And we should be able to modify a collection to have a validator using 3.6 query
        // features.
        assert.commandWorked(testDB.runCommand({collMod: "coll", validator: validator}));

        // Set the feature compatibility version to 3.4 and then restart with
        // internalValidateFeaturesAsMaster=false.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({
            dbpath: dbpath,
            binVersion: "latest",
            noCleanData: true,
            setParameter: "internalValidateFeaturesAsMaster=false"
        });
        assert.neq(null, conn, "mongod was unable to start up");

        testDB = conn.getDB(testName);

        // Even though the feature compatibility version is 3.4, we should still be able to add a
        // validator using 3.6 query features, because internalValidateFeaturesAsMaster is false.
        assert.commandWorked(testDB.createCollection("coll3", {validator: validator}));

        // We should also be able to modify a collection to have a validator using 3.6 query
        // features.
        testDB.coll3.drop();
        assert.commandWorked(testDB.createCollection("coll3"));
        assert.commandWorked(testDB.runCommand({collMod: "coll3", validator: validator}));

        MongoRunner.stopMongod(conn);
    }

    testValidator({$jsonSchema: {properties: {foo: {type: "string"}}}}, {foo: 1.0});
    testValidator({$expr: {$eq: ["$a", "good"]}}, {a: "bad"});
}());
