// SERVER-25942 Test that views are not validated in the case that only collections are queried.
(function() {
    db = db.getSiblingDB('list_collections_no_views');

    assert.commandWorked(db.createCollection('foo'));
    assert.commandWorked(db.createView('bar', 'foo', []));

    let all = db.runCommand({listCollections: 1});
    assert.commandWorked(all);

    let allExpected = [
        {
          "name": "foo",
          "type": "collection",
        },
        {
          "name": "system.views",
          "type": "collection",
        },
        {
          "name": "bar",
          "type": "view",
        }
    ];

    assert.eq(allExpected,
              all.cursor.firstBatch
                  .filter(function(c) {
                      { return c.name != "system.indexes"; }
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  }));

    // {type: {$exists: false}} is needed for versions <= 3.2
    let collOnlyCommand = {
        listCollections: 1,
        filter: {$or: [{type: 'collection'}, {type: {$exists: false}}]}
    };

    let collOnly = db.runCommand(collOnlyCommand);
    assert.commandWorked(collOnly);

    let collOnlyExpected = [
        {
          "name": "foo",
          "type": "collection",
        },
        {
          "name": "system.views",
          "type": "collection",
        },
    ];

    assert.eq(collOnlyExpected,
              collOnly.cursor.firstBatch
                  .filter(function(c) {
                      { return c.name != "system.indexes"; }
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  }));

    let viewOnly = db.runCommand({listCollections: 1, filter: {type: 'view'}});
    assert.commandWorked(viewOnly);
    let viewOnlyExpected = [{
        "name": "bar",
        "type": "view",
    }];

    assert.eq(viewOnlyExpected,
              viewOnly.cursor.firstBatch
                  .filter(function(c) {
                      { return c.name != "system.indexes"; }
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  }));

    db.system.views.insertOne({invalid: NumberLong(1000)});

    let collOnlyInvalidView = db.runCommand(collOnlyCommand);
    assert.eq(collOnlyExpected,
              collOnlyInvalidView.cursor.firstBatch
                  .filter(function(c) {
                      { return c.name != "system.indexes"; }
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  }));

    assert.commandFailed(db.runCommand({listCollections: 1}));
    assert.commandFailed(db.runCommand({listCollections: 1, filter: {type: 'view'}}));

    // Fix database state for end of test validation
    db.system.views.deleteOne({invalid: NumberLong(1000)});
})();
