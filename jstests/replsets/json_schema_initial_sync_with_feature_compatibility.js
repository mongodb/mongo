/**
 * Test that a new replica set member can successfully sync a collection with a $jsonSchema
 * validator, even when the replica set was downgraded to feature compatibility version 3.4.
 */

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    var testName = "json_schema_initial_sync_with_feature_compatibility";

    //
    // Create a single-node replica set.
    //
    var replTest = new ReplSetTest({name: testName, nodes: 1});

    var conns = replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var adminDB = primary.getDB("admin");
    var testDB = primary.getDB("test");

    //
    // Explicitly set the replica set to feature compatibility version 3.6.
    //
    var res;
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.6", res.featureCompatibilityVersion);

    //
    // Create and populate a collection with a $jsonSchema validator.
    //
    assert.commandWorked(testDB.createCollection(
        "coll", {validator: {$jsonSchema: {properties: {str: {type: "string"}}}}}));

    var bulk = testDB.coll.initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({date: new Date(), x: i, str: "all the talk on the market"});
    }
    assert.writeOK(bulk.execute());

    //
    // Downgrade the replica set to feature compatibility version 3.4.
    //
    var res;
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.4", res.featureCompatibilityVersion);

    //
    // Add a new member to the replica set.
    //
    var secondaryDBPath = MongoRunner.dataPath + testName + "_secondary";
    resetDbpath(secondaryDBPath);
    var secondary = replTest.add({dbpath: secondaryDBPath});
    replTest.reInitiate(secondary);
    reconnect(primary);
    reconnect(secondary);

    //
    // Once the new member completes its initial sync, stop it, remove it from the replica set, and
    // start it back up as an individual instance.
    //
    replTest.waitForState(secondary, [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

    replTest.stopSet(undefined /* send default signal */, true /* don't clear data directory */);

    secondary = MongoRunner.runMongod({dbpath: secondaryDBPath, noCleanData: true});

    //
    // Verify that the $jsonSchema validator synced to the new member by attempting to insert a
    // document that does not validate and checking that the insert fails.
    //
    var secondaryDB = secondary.getDB("test");
    assert.writeError(secondaryDB.coll.insert({str: 1.0}), ErrorCodes.DocumentValidationFailure);

    //
    // Verify that, even though the existing $jsonSchema validator still works, it is not possible
    // to create a new $jsonSchema validator because of feature compatibility 3.4.
    //
    assert.commandFailed(secondaryDB.runCommand({collMod: "coll", validator: {$jsonSchema: {}}}),
                         ErrorCodes.InvalidOptions);
}());