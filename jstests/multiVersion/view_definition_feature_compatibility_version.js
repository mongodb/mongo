/**
 * Test that mongod will not allow creation of a view using 3.6 query features when the feature
 * compatibility version is older than 3.6.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    const testName = "view_definition_feature_compatibility_version_multiversion";
    const dbpath = MongoRunner.dataPath + testName;

    /**
     * Tests the correct behavior of view creation and modification using 3.6 query features with
     * different binary versions and feature compatibility versions.
     *
     * TODO SERVER-31588: Remove FCV 3.4 validation during the 3.7 development cycle.
     */
    function testView(pipeline) {
        resetDbpath(dbpath);

        let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
        assert.neq(null, conn, "mongod was unable to start up");
        let testDB = conn.getDB(testName);

        // Explicitly set feature compatibility version 3.6.
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

        // Create a view using a 3.6 query feature.
        assert.commandWorked(testDB.createView("view1", "coll", pipeline));

        // Update an existing view to use a 3.6 query feature.
        assert(testDB.view1.drop(), "Drop of view failed");
        assert.commandWorked(testDB.createView("view1", "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "view1", viewOn: "coll", pipeline: pipeline}));

        // Create an empty view which we will attempt to update to use 3.6 query features under
        // feature compatibility mode 3.4.
        assert.commandWorked(testDB.createView("view2", "coll", []));

        // Set the feature compatibility version to 3.4.
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

        // Read against an existing view using 3.6 query features should not fail.
        assert.commandWorked(testDB.runCommand({find: "view1"}));

        // Trying to create a new view using 3.6 query features should fail.
        assert.commandFailedWithCode(testDB.createView("view_fail", "coll", pipeline),
                                     ErrorCodes.QueryFeatureNotAllowed);

        // Trying to update existing view to use 3.6 query features should also fail.
        assert.commandFailedWithCode(
            testDB.runCommand({collMod: "view2", viewOn: "coll", pipeline: pipeline}),
            ErrorCodes.QueryFeatureNotAllowed);

        MongoRunner.stopMongod(conn);

        // Starting up a 3.4 mongod with 3.6 query features will succeed.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4", noCleanData: true});
        assert.neq(null, conn, "mongod 3.4 was unable to start up");
        testDB = conn.getDB(testName);

        // Reads will fail against views with 3.6 query features when running a 3.4 binary.
        // Not checking the code returned on failure as it is not uniform across the various
        // 'pipeline' arguments tested.
        assert.commandFailed(testDB.runCommand({find: "view1"}));

        // A read against a view that does not contain 3.6 query features succeeds.
        assert.commandWorked(testDB.runCommand({find: "view2"}));

        MongoRunner.stopMongod(conn);

        // Starting up a 3.6 mongod should succeed, even though the feature compatibility version is
        // still set to 3.4.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest", noCleanData: true});
        assert.neq(null, conn, "mongod was unable to start up");
        testDB = conn.getDB(testName);

        // A read against a view with 3.6 query features should now succeed.
        assert.commandWorked(testDB.runCommand({find: "view1"}));

        // Set the feature compatibility version back to 3.6.
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

        // Now we should be able to create a view using 3.6 query features again.
        assert.commandWorked(testDB.createView("view3", "coll", pipeline));

        // And we should be able to modify a view to use 3.6 query features.
        assert(testDB.view3.drop(), "Drop of view failed");
        assert.commandWorked(testDB.createView("view3", "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "view3", viewOn: "coll", pipeline: pipeline}));

        // Set the feature compatibility version to 3.4 and then restart with
        // internalValidateFeaturesAsMaster=false.
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({
            dbpath: dbpath,
            binVersion: "latest",
            noCleanData: true,
            setParameter: "internalValidateFeaturesAsMaster=false"
        });
        assert.neq(null, conn, "mongod was unable to start up");
        testDB = conn.getDB(testName);

        // Even though the feature compatibility version is 3.4, we should still be able to create a
        // view using 3.6 query features, because internalValidateFeaturesAsMaster is false.
        assert.commandWorked(testDB.createView("view4", "coll", pipeline));

        // We should also be able to modify a view to use 3.6 query features.
        assert(testDB.view4.drop(), "Drop of view failed");
        assert.commandWorked(testDB.createView("view4", "coll", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "view4", viewOn: "coll", pipeline: pipeline}));

        MongoRunner.stopMongod(conn);

        // Starting up a 3.4 mongod with 3.6 query features.
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "3.4", noCleanData: true});
        assert.neq(null, conn, "mongod 3.4 was unable to start up");
        testDB = conn.getDB(testName);

        // Existing views with 3.6 query features can be dropped.
        assert(testDB.view1.drop(), "Drop of view failed");
        assert(testDB.system.views.drop(), "Drop of system.views collection failed");

        MongoRunner.stopMongod(conn);
    }

    testView([{$match: {$expr: {$eq: ["$x", "$y"]}}}]);
    testView([{$match: {$jsonSchema: {properties: {foo: {type: "string"}}}}}]);
    testView([{$facet: {field: [{$match: {$jsonSchema: {properties: {foo: {type: "string"}}}}}]}}]);
    testView([{$facet: {field: [{$match: {$expr: {$eq: ["$x", "$y"]}}}]}}]);
    testView([{$lookup: {from: "foreign", as: "as", pipeline: []}}]);
}());
