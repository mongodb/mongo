/**
 * Tests the behavior of views when its backing collection is dropped, as well as the behavior of
 * system.views when views are dropped.
 * @tags: [requires_find_command]
 */
(function() {
    "use strict";

    let viewsDBName = "views_drop";
    let viewsDB = db.getSiblingDB(viewsDBName);
    viewsDB.dropDatabase();

    // Create collection and a view on it.
    assert.writeOK(viewsDB.coll.insert({x: 1}));
    assert.commandWorked(viewsDB.createView("view", "coll", []));
    assert.eq(
        viewsDB.view.find({}, {_id: 0}).toArray(), [{x: 1}], "couldn't find expected doc in view");

    // Drop collection, view and system.views in that order, checking along the way.
    assert(viewsDB.coll.drop(), "couldn't drop coll");
    assert.eq(viewsDB.view.find().toArray(), [], "view isn't empty after dropping coll");
    assert(viewsDB.view.drop(), "couldn't drop view");
    assert.eq(
        viewsDB.system.views.find().toArray(), [], "system.views isn't empty after dropping view");
    assert(viewsDB.system.views.drop(), "couldn't drop system.views");

    // Database should now be empty.
    let res = viewsDB.runCommand({listCollections: 1});
    assert.commandWorked(res);
    assert.eq(res.cursor.firstBatch.filter((entry) => entry.name != "system.indexes"),
              [],
              viewsDBName + " is not empty after deleting views and system.views");
})();
