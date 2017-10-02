/**
 * Test that a new replica set member can successfully sync a collection with a validator using 3.6
 * query features, even when the replica set was downgraded to feature compatibility version 3.4.
 *
 * We restart the secondary as part of this test with the expecation that it still has the same
 * data after the restart.
 * @tags: [requires_persistence]
 */

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    const testName = "collection_validator_initial_sync_with_feature_compatibility";

    function testValidator(validator, nonMatchingDocument) {
        //
        // Create a single-node replica set.
        //
        let replTest = new ReplSetTest({name: testName, nodes: 1});

        replTest.startSet();
        replTest.initiate();

        let primary = replTest.getPrimary();
        let testDB = primary.getDB("test");

        //
        // Explicitly set the replica set to feature compatibility version 3.6.
        //
        assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.6"}));

        //
        // Create a collection with a validator using 3.6 query features.
        //
        assert.commandWorked(testDB.createCollection("coll", {validator: validator}));

        //
        // Downgrade the replica set to feature compatibility version 3.4.
        //
        assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

        //
        // Add a new member to the replica set.
        //
        let secondaryDBPath = MongoRunner.dataPath + testName + "_secondary";
        resetDbpath(secondaryDBPath);
        let secondary = replTest.add({dbpath: secondaryDBPath});
        replTest.reInitiate(secondary);
        reconnect(primary);
        reconnect(secondary);

        //
        // Once the new member completes its initial sync, stop it, remove it from the replica
        // set, and start it back up as an individual instance.
        //
        replTest.waitForState(secondary, [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

        replTest.stopSet(undefined /* send default signal */,
                         true /* don't clear data directory */);

        secondary = MongoRunner.runMongod({dbpath: secondaryDBPath, noCleanData: true});
        assert.neq(null, secondary, "mongod was unable to start up");

        //
        // Verify that the validator synced to the new member by attempting to insert a document
        // that does not validate and checking that the insert fails.
        //
        let secondaryDB = secondary.getDB("test");
        assert.writeError(secondaryDB.coll.insert(nonMatchingDocument),
                          ErrorCodes.DocumentValidationFailure);

        //
        // Verify that, even though the existing validator still works, it is not possible to
        // create a new validator using 3.6 query features because of feature compatibility version
        // 3.4.
        //
        assert.commandFailedWithCode(
            secondaryDB.runCommand({collMod: "coll", validator: validator}),
            ErrorCodes.QueryFeatureNotAllowed);

        MongoRunner.stopMongod(secondary);
    }

    testValidator({$jsonSchema: {properties: {foo: {type: "string"}}}}, {foo: 1.0});
    testValidator({$expr: {$eq: ["$a", "good"]}}, {a: "bad"});
}());