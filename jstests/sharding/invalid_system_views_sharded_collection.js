/**
 * Tests that invalid view definitions in system.views do not impact valid commands on sharded
 * collections.
 */

(function() {
"use strict";

function runTest(st, badViewDefinition) {
    const mongos = st.s;
    const config = mongos.getDB("config");
    const db = mongos.getDB("invalid_system_views");
    assert.commandWorked(db.dropDatabase());

    assert.commandWorked(config.adminCommand({enableSharding: db.getName()}));
    st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

    // Create sharded and unsharded collections, then insert an invalid view into system.views.
    const viewsCollection = db.getCollection("coll");
    const staticCollection = db.getCollection("staticCollection");
    assert.commandWorked(
        config.adminCommand({shardCollection: viewsCollection.getFullName(), key: {a: 1}}));
    assert.commandWorked(
        config.adminCommand({shardCollection: staticCollection.getFullName(), key: {a: 1}}));

    assert.commandWorked(viewsCollection.createIndex({x: 1}));

    const unshardedColl = db.getCollection("unshardedColl");
    assert.commandWorked(unshardedColl.insert({b: "boo"}));

    assert.commandWorked(db.system.views.insert(badViewDefinition),
                         "failed to insert " + tojson(badViewDefinition));

    // Test that a command involving views properly fails with a views-specific error code.
    assert.commandFailedWithCode(
        db.runCommand({listCollections: 1}),
        ErrorCodes.InvalidViewDefinition,
        "listCollections should have failed in the presence of an invalid view");

    // Helper function to create a message to use if an assertion fails.
    function makeErrorMessage(msg) {
        return msg +
            " should work on a valid, existing collection, despite the presence of bad views" +
            " in system.views";
    }

    assert.commandWorked(viewsCollection.insert({y: "baz", a: 5}), makeErrorMessage("insert"));

    assert.commandWorked(viewsCollection.update({y: "baz"}, {$set: {y: "qux"}}),
                         makeErrorMessage("update"));

    assert.commandWorked(viewsCollection.remove({y: "baz"}), makeErrorMessage("remove"));

    assert.commandWorked(
        db.runCommand(
            {findAndModify: viewsCollection.getName(), query: {x: 1, a: 1}, update: {x: 2}}),
        makeErrorMessage("findAndModify with update"));

    assert.commandWorked(
        db.runCommand(
            {findAndModify: viewsCollection.getName(), query: {x: 2, a: 1}, remove: true}),
        makeErrorMessage("findAndModify with remove"));

    const lookup = {
        $lookup:
            {from: unshardedColl.getName(), localField: "_id", foreignField: "_id", as: "match"}
    };
    assert.commandWorked(
        db.runCommand({aggregate: viewsCollection.getName(), pipeline: [lookup], cursor: {}}),
        makeErrorMessage("aggregate with $lookup"));

    const graphLookup = {
            $graphLookup: {
                from: unshardedColl.getName(),
                startWith: "$_id",
                connectFromField: "_id",
                connectToField: "_id",
                as: "match"
            }
        };
    assert.commandWorked(
        db.runCommand({aggregate: viewsCollection.getName(), pipeline: [graphLookup], cursor: {}}),
        makeErrorMessage("aggregate with $graphLookup"));

    assert.commandWorked(db.runCommand({dropIndexes: viewsCollection.getName(), index: "x_1"}),
                         makeErrorMessage("dropIndexes"));

    assert.commandWorked(viewsCollection.createIndex({x: 1}), makeErrorMessage("createIndexes"));

    assert.commandWorked(
        db.runCommand({collMod: viewsCollection.getName(), validator: {x: {$type: "string"}}}),
        makeErrorMessage("collMod"));

    assert.commandWorked(db.runCommand({drop: viewsCollection.getName()}),
                         makeErrorMessage("drop"));
    assert.commandWorked(db.runCommand({drop: staticCollection.getName()}),
                         makeErrorMessage("drop"));

    // An invalid view in the view catalog should not interfere with attempting to run operations on
    // nonexistent collections.
    assert.commandWorked(db.runCommand({drop: staticCollection.getName()}),
                         makeErrorMessage("drop"));

    assert.commandWorked(db.runCommand({drop: unshardedColl.getName()}), makeErrorMessage("drop"));

    // Drop the offending view so that the validate hook succeeds.
    db.system.views.remove(badViewDefinition);
}

const st = new ShardingTest({name: "views_sharded", shards: 2, other: {enableBalancer: false}});

runTest(st, {_id: "invalid_system_views.badViewStringPipeline", viewOn: "coll", pipeline: "bad"});
runTest(st, {_id: "invalid_system_views.badViewEmptyObjectPipeline", viewOn: "coll", pipeline: {}});
runTest(st, {_id: "invalid_system_views.badViewNumericalPipeline", viewOn: "coll", pipeline: 7});
runTest(
    st,
    {_id: "invalid_system_views.badViewArrayWithIntegerPipeline", viewOn: "coll", pipeline: [1]});
runTest(st, {
    _id: "invalid_system_views.badViewArrayWithEmptyArrayPipeline",
    viewOn: "coll",
    pipeline: [[]]
});
runTest(st, {_id: 7, viewOn: "coll", pipeline: []});
runTest(st, {_id: "invalid_system_views.embedded\0null", viewOn: "coll", pipeline: []});
runTest(st, {_id: "invalidNotFullyQualifiedNs", viewOn: "coll", pipeline: []});
runTest(st, {_id: "invalid_system_views.missingViewOnField", pipeline: []});

st.stop();
}());
