/**
 * Tests the behavior of views when the backing view or collection is changed.
 * @tags: [requires_find_command]
 */
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
        assert.commandWorked(viewDB.runCommand(
            {create: "view", viewOn: "collection", pipeline: [{$match: {a: 1}}]}));
        assert.commandWorked(viewDB.runCommand(
            {create: "viewOnView", viewOn: "view", pipeline: [{$match: {b: 1}}]}));
    };
    let assertFindResultEq = function(collName, expected) {
        let res = viewDB.runCommand({find: collName, filter: {}, projection: {_id: 0, a: 1, b: 1}});
        assert.commandWorked(res);
        let arr = new DBCommandCursor(db.getMongo(), res).toArray();
        let errmsg = tojson({expected: expected, got: arr});
        assert(arrayEq(arr, expected), errmsg);
    };

    let doc = {a: 1, b: 1};

    resetCollectionAndViews();

    // A view is updated when its viewOn is modified. When auth is enabled, we expect collMod to
    // fail when specifying "viewOn" but not "pipeline".
    assert.writeOK(collection.insert(doc));
    assertFindResultEq("view", [doc]);
    let res = viewDB.runCommand({collMod: "view", viewOn: "nonexistent"});
    if (jsTest.options().auth) {
        assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
    } else {
        assert.commandWorked(res);
        assertFindResultEq("view", []);
    }

    resetCollectionAndViews();

    // A view is updated when its pipeline is modified. When auth is enabled, we expect collMod to
    // fail when specifying "pipeline" but not "viewOn".
    assert.writeOK(collection.insert(doc));
    assert.writeOK(collection.insert({a: 7}));
    assertFindResultEq("view", [doc]);
    res = viewDB.runCommand({collMod: "view", pipeline: [{$match: {a: {$gt: 4}}}]});
    if (jsTest.options().auth) {
        assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
    } else {
        assert.commandWorked(res);
        assertFindResultEq("view", [{a: 7}]);
    }

    resetCollectionAndViews();

    // A view is updated when the backing collection is updated.
    assert.writeOK(collection.insert(doc));
    assertFindResultEq("view", [doc]);
    assert.writeOK(collection.update({a: 1}, {$set: {a: 2}}));
    assertFindResultEq("view", []);

    resetCollectionAndViews();

    // A view is updated when a backing view is updated.
    assert.writeOK(collection.insert(doc));
    assertFindResultEq("viewOnView", [doc]);
    assert.commandWorked(viewDB.runCommand(
        {collMod: "view", viewOn: "collection", pipeline: [{$match: {nonexistent: 1}}]}));
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
