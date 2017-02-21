// Test the creation of view with a duplicate name to a collection.

(function() {
    "use strict";

    const dbName = "views_duplicate_ns";
    const viewsDb = db.getSiblingDB(dbName);
    const collName = "myns";
    const viewId = dbName + "." + collName;

    assert.commandWorked(viewsDb.dropDatabase());
    assert.writeOK(viewsDb.system.views.remove({_id: viewId}));
    assert.commandWorked(viewsDb.runCommand({create: collName}));
    assert.writeOK(viewsDb.system.views.insert({
        _id: viewId,
        viewOn: "coll",
        pipeline: [],
    }));
    assert.eq(2,
              viewsDb.getCollectionInfos()
                  .filter(coll => {
                      return coll.name === collName;
                  })
                  .length);
}());