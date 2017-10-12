/**
 * Test that a new replica set member can successfully sync a collection with a view using 3.6 query
 * features, even when the replica set was downgraded to feature compatibility version 3.4.
 *
 * We restart the secondary as part of this test with the expectation that it still has the same
 * data after the restart.
 * @tags: [requires_persistence]
 *
 * TODO SERVER-31588: Remove FCV 3.4 validation during the 3.7 development cycle.
 */

load("jstests/replsets/rslib.js");

(function() {
    "use strict";
    const testName = "view_definition_initial_sync_with_feature_compatibility";

    function testView(pipeline) {
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
        // Create a view using 3.6 query features.
        //
        assert.commandWorked(testDB.createView("view1", "coll", pipeline));

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
        // Verify that the view synced to the new member.
        //
        let secondaryDB = secondary.getDB("test");
        assert.eq(secondaryDB.system.views.findOne({_id: "test.view1"}, {_id: 1}),
                  {_id: "test.view1"});

        //
        // Verify that, even though a view using 3.6 query features exists, it is not possible to
        // create a new view using 3.6 query features because of feature compatibility version 3.4.
        //
        assert.commandFailedWithCode(secondaryDB.createView("view2", "coll", pipeline),
                                     ErrorCodes.QueryFeatureNotAllowed);

        MongoRunner.stopMongod(secondary);
    }

    testView([{$match: {$expr: {$eq: ["$x", "$y"]}}}]);
    testView([{$match: {$jsonSchema: {properties: {foo: {type: "string"}}}}}]);
    testView([{$facet: {field: [{$match: {$jsonSchema: {properties: {foo: {type: "string"}}}}}]}}]);
    testView([{$facet: {field: [{$match: {$expr: {$eq: ["$x", "$y"]}}}]}}]);
    testView([{$lookup: {from: "foreign", as: "as", pipeline: []}}]);
}());