
(function() {
    "use strict";

    let systemDBName = "system_collections_drop";
    let systemDB = db.getSiblingDB(systemDBName);
    systemDB.dropDatabase();

    // Create collection and a view on it.
    assert.writeOK(systemDB.system.js.insert({x: 1}));
    assert.commandWorked(viewsDB.createView("view", "coll", []));
    assert.eq(
        viewsDB.view.find({}, {_id: 0}).toArray(), [{x: 1}], "couldn't find expected doc in view");
})();
