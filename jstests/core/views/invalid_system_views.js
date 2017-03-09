/**
 * Tests that invalid view definitions in system.views do not impact valid commands on existing
 * collections.
 */
(function() {
    "use strict";

    function runTest(badViewDefinition) {
        let viewsDB = db.getSiblingDB("invalid_system_views");
        assert.commandWorked(viewsDB.dropDatabase());

        // Create a regular collection, then insert an invalid view into system.views.
        assert.writeOK(viewsDB.collection.insert({x: 1}));
        assert.commandWorked(viewsDB.runCommand({create: "collection2"}));
        assert.commandWorked(viewsDB.runCommand({create: "collection3"}));
        assert.commandWorked(viewsDB.collection.createIndex({x: 1}));
        assert.writeOK(viewsDB.system.views.insert(badViewDefinition),
                       "failed to insert " + tojson(badViewDefinition));

        // Test that a command involving views properly fails with a views-specific error code.
        assert.commandFailedWithCode(
            viewsDB.runCommand({listCollections: 1}),
            ErrorCodes.InvalidViewDefinition,
            "listCollections should have failed in the presence of an invalid view");

        // Helper function to create a message to use if an assertion fails.
        function makeErrorMessage(msg) {
            return msg +
                " should work on a valid, existing collection, despite the presence of bad views" +
                " in system.views";
        }

        // Commands that run on existing regular collections should not be impacted by the presence
        // of invalid views.
        assert.commandWorked(
            db.adminCommand(
                {applyOps: [{op: "c", ns: "invalid_system_views.$cmd", o: {drop: "collection3"}}]}),
            makeErrorMessage("applyOps"));

        assert.writeOK(viewsDB.collection.insert({y: "baz"}), makeErrorMessage("insert"));

        assert.writeOK(viewsDB.collection.update({y: "baz"}, {$set: {y: "qux"}}),
                       makeErrorMessage("update"));

        assert.writeOK(viewsDB.collection.remove({y: "baz"}), makeErrorMessage("remove"));

        assert.commandWorked(
            viewsDB.runCommand({findAndModify: "collection", query: {x: 1}, update: {x: 2}}),
            makeErrorMessage("findAndModify with update"));

        assert.commandWorked(
            viewsDB.runCommand({findAndModify: "collection", query: {x: 2}, remove: true}),
            makeErrorMessage("findAndModify with remove"));

        const lookup = {
            $lookup: {from: "collection2", localField: "_id", foreignField: "_id", as: "match"}
        };
        assert.commandWorked(
            viewsDB.runCommand({aggregate: "collection", pipeline: [lookup], cursor: {}}),
            makeErrorMessage("aggregate with $lookup"));

        const graphLookup = {
            $graphLookup: {
                from: "collection2",
                startWith: "$_id",
                connectFromField: "_id",
                connectToField: "_id",
                as: "match"
            }
        };
        assert.commandWorked(
            viewsDB.runCommand({aggregate: "collection", pipeline: [graphLookup], cursor: {}}),
            makeErrorMessage("aggregate with $graphLookup"));

        assert.commandWorked(viewsDB.runCommand({dropIndexes: "collection", index: "x_1"}),
                             makeErrorMessage("dropIndexes"));

        assert.commandWorked(viewsDB.collection.createIndex({x: 1}),
                             makeErrorMessage("createIndexes"));

        assert.commandWorked(viewsDB.collection.reIndex(), makeErrorMessage("reIndex"));

        const storageEngine = jsTest.options().storageEngine;
        if (storageEngine === "ephemeralForTest" || storageEngine === "inMemory") {
            print("Not testing compact command on ephemeral storage engine " + storageEngine);
        } else {
            assert.commandWorked(viewsDB.runCommand({compact: "collection", force: true}),
                                 makeErrorMessage("compact"));
        }

        assert.commandWorked(
            viewsDB.runCommand({collMod: "collection", validator: {x: {$type: "string"}}}),
            makeErrorMessage("collMod"));

        const renameCommand = {
            renameCollection: "invalid_system_views.collection",
            to: "invalid_system_views.collection2",
            dropTarget: true
        };
        assert.commandWorked(viewsDB.adminCommand(renameCommand),
                             makeErrorMessage("renameCollection"));

        assert.commandWorked(viewsDB.runCommand({drop: "collection2"}), makeErrorMessage("drop"));

        // Drop the offending view so that the validate hook succeeds.
        assert.writeOK(viewsDB.system.views.remove(badViewDefinition));
    }

    let badViews = [
        {_id: "badViewStringPipeline", pipeline: "bad"},
        {_id: "badViewEmptyObjectPipeline", pipeline: {}},
        {_id: "badViewNumericalPipeline", pipeline: 7},
        {_id: "badViewArrayWithIntegerPipeline", pipeline: [1]},
        {_id: "badViewArrayWithEmptyObjectPipeline", pipeline: [{}]},
        {_id: "badViewArrayWithEmptyArrayPipeline", pipeline: [[]]},
        {_id: 7, pipeline: []},
        {_id: "invalid_system_views.embedded\0null", viewOn: "collection", pipeline: []}
    ];
    badViews.forEach(runTest);
}());
