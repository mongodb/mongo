/**
 * Test that a new replica set member can successfully sync a collection with a view using 4.2
 * aggregation features, even when the replica set was downgraded to feature compatibility version
 * 4.0.
 *
 * TODO SERVER-41273: Remove FCV 4.0 validation during the 4.3 development cycle.
 *
 * We restart the secondary as part of this test with the expectation that it still has the same
 * data after the restart.
 * @tags: [requires_persistence]
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
    // Explicitly set the replica set to feature compatibility version 4.2.
    //
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

    //
    // Create a view using 4.2 query features.
    //
    assert.commandWorked(testDB.createView("view1", "coll", pipeline));

    //
    // Downgrade the replica set to feature compatibility version 4.0.
    //
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

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

    replTest.stopSet(undefined /* send default signal */, true /* don't clear data directory */);

    secondary = MongoRunner.runMongod({dbpath: secondaryDBPath, noCleanData: true});
    assert.neq(null, secondary, "mongod was unable to start up");

    //
    // Verify that the view synced to the new member.
    //
    let secondaryDB = secondary.getDB("test");
    assert.eq(secondaryDB.system.views.findOne({_id: "test.view1"}, {_id: 1}), {_id: "test.view1"});

    //
    // Verify that, even though a view using 4.2 query features exists, it is not possible to
    // create a new view using 4.2 query features because of feature compatibility version 4.0.
    //
    assert.commandFailedWithCode(secondaryDB.createView("view2", "coll", pipeline),
                                 ErrorCodes.QueryFeatureNotAllowed);

    MongoRunner.stopMongod(secondary);
}

testView([{$addFields: {x: {$round: 4.57}}}]);
testView([{$addFields: {x: {$trunc: [4.57, 1]}}}]);
testView([{$addFields: {x: {$regexFind: {input: "string", regex: /st/}}}}]);
testView([{$addFields: {x: {$regexFindAll: {input: "string", regex: /st/}}}}]);
testView([{$addFields: {x: {$regexMatch: {input: "string", regex: /st/}}}}]);
testView([{$facet: {pipe1: [{$addFields: {x: {$round: 4.57}}}]}}]);
testView([{
    $facet: {
        pipe1: [{$addFields: {x: {$round: 4.57}}}],
        pipe2: [{$addFields: {newThing: {$regexMatch: {input: "string", regex: /st/}}}}]
    }
}]);
testView([{$lookup: {from: 'x', pipeline: [{$addFields: {x: {$round: 4.57}}}], as: 'results'}}]);
testView([{
        $graphLookup: {
            from: 'x',
            startWith: ["$_id"],
            connectFromField: "target_id",
            connectToField: "_id",
            restrictSearchWithMatch: {$expr: {$eq: [4, {$round: "$x"}]}},
            as: 'results'
        }
    }]);
testView([{
        $lookup: {
            from: 'x',
            pipeline: [{$facet: {pipe1: [{$addFields: {x: {$round: 4.57}}}]}}],
            as: 'results'
        }
    }]);
}());
