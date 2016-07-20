// Tests the behavior of views when the backing view or collection is changed.

(function() {
    "use strict";

    // For arrayEq.
    load("jstests/aggregation/extras/utils.js");

    let viewDB = db.getSiblingDB("views_change");
    let collection = viewDB.collection;
    let view = viewDB.view;
    let viewOnView = viewDB.viewOnView;

    // Convenience functions.
    let resetCollectionAndViews = function() {
        viewDB.runCommand({drop: "collection"});
        viewDB.runCommand({drop: "view"});
        viewDB.runCommand({drop: "viewOnView"});
        assert.commandWorked(viewDB.runCommand({create: "collection"}));
        assert.commandWorked(viewDB.runCommand({
            create: "view",
            viewOn: "collection",
            projection: {_id: 0},
            pipeline: [{$match: {a: 1}}]
        }));
        assert.commandWorked(viewDB.runCommand(
            {create: "viewOnView", viewOn: "view", pipeline: [{$match: {b: 1}}]}));
    };
    let assertFindResultEq = function(collName, expected) {
        let res = viewDB.runCommand({find: collName, filter: {}, projection: {_id: 0, a: 1, b: 1}});
        assert.commandWorked(res);
        let cursor = new DBCommandCursor(db.getMongo(), res);
        assert(arrayEq(cursor.toArray(), expected));
    };

    let doc = {a: 1, b: 1};

    resetCollectionAndViews();

    // A view is updated when the backing collection is updated.
    assert.writeOK(collection.insert(doc));
    printjson(viewDB.collection.find().toArray());
    printjson(viewDB.view.find().toArray());
    assertFindResultEq("view", [doc]);
    assert.writeOK(collection.update({a: 1}, {$set: {a: 2}}));
    assertFindResultEq("view", []);

    resetCollectionAndViews();

    // A view is updated when a backing view is updated.
    assert.writeOK(collection.insert(doc));
    assertFindResultEq("viewOnView", [doc]);
    assert.commandWorked(
        viewDB.runCommand({collMod: "view", pipeline: [{$match: {nonexistent: 1}}]}));
    assertFindResultEq("viewOnView", []);

    resetCollectionAndViews();

    // A view appears empty if the backing collection is dropped.
    assert.writeOK(collection.insert(doc));
    assertFindResultEq("view", [doc]);
    assert.commandWorked(viewDB.runCommand({drop: "collection"}));
    assertFindResultEq("view", []);

    resetCollectionAndViews();

    // A view appears empty if a backing view is dropped.
    assert.writeOK(collection.insert(doc));
    assertFindResultEq("viewOnView", [doc]);
    assert.commandWorked(viewDB.runCommand({drop: "view"}));
    assertFindResultEq("viewOnView", []);
}());
