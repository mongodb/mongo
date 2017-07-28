(function() {
    // SERVER-30406 Test that renaming system.views correctly invalidates the view catalog
    'use strict';
    db.view.drop();
    db.coll.drop();
    assert.commandWorked(db.createView("view", "coll", []));
    assert.writeOK(db.coll.insert({_id: 1}));
    assert.eq(db.view.find().count(), 1, "couldn't find document in view");
    assert.commandWorked(db.system.views.renameCollection("views", /*dropTarget*/ true));
    assert.eq(db.view.find().count(),
              0,
              "find on view should have returned no results after renaming away system.views");
    assert.commandWorked(db.views.renameCollection("system.views"));
    assert.eq(db.view.find().count(),
              1,
              "find on view should have worked again after renaming system.views back in place");
})();
