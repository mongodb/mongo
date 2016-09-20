// SERVER-25942 Test that views are not validated in the case that only collections are queried.
(function() {
    'use strict';
    let mydb = db.getSiblingDB('list_collections_no_views');

    assert.commandWorked(mydb.createCollection('foo'));
    assert.commandWorked(mydb.createView('bar', 'foo', []));

    let all = mydb.runCommand({listCollections: 1});
    assert.commandWorked(all);

    let allExpected = [
        {
          "name": "bar",
          "type": "view",
        },
        {
          "name": "foo",
          "type": "collection",
        },
        {
          "name": "system.views",
          "type": "collection",
        },
    ];

    assert.eq(allExpected,
              all.cursor.firstBatch
                  .filter(function(c) {
                      return c.name != "system.indexes";
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  })
                  .sort(function(c1, c2) {
                      if (c1.name > c2.name) {
                          return 1;
                      }

                      if (c1.name < c2.name) {
                          return -1;
                      }

                      return 0;
                  }));

    // {type: {$exists: false}} is needed for versions <= 3.2
    let collOnlyCommand = {
        listCollections: 1,
        filter: {$or: [{type: 'collection'}, {type: {$exists: false}}]}
    };

    let collOnly = mydb.runCommand(collOnlyCommand);
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
                      return c.name != "system.indexes";
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  })
                  .sort(function(c1, c2) {
                      if (c1.name > c2.name) {
                          return 1;
                      }

                      if (c1.name < c2.name) {
                          return -1;
                      }

                      return 0;
                  }));

    let viewOnly = mydb.runCommand({listCollections: 1, filter: {type: 'view'}});
    assert.commandWorked(viewOnly);
    let viewOnlyExpected = [{
        "name": "bar",
        "type": "view",
    }];

    assert.eq(viewOnlyExpected,
              viewOnly.cursor.firstBatch
                  .filter(function(c) {
                      return c.name != "system.indexes";
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  })
                  .sort(function(c1, c2) {
                      if (c1.name > c2.name) {
                          return 1;
                      }

                      if (c1.name < c2.name) {
                          return -1;
                      }

                      return 0;
                  }));

    let views = mydb.getCollection('system.views');
    views.insertOne({invalid: NumberLong(1000)});

    let collOnlyInvalidView = mydb.runCommand(collOnlyCommand);
    assert.eq(collOnlyExpected,
              collOnlyInvalidView.cursor.firstBatch
                  .filter(function(c) {
                      return c.name != "system.indexes";
                  })
                  .map(function(c) {
                      return {name: c.name, type: c.type};
                  })
                  .sort(function(c1, c2) {
                      if (c1.name > c2.name) {
                          return 1;
                      }

                      if (c1.name < c2.name) {
                          return -1;
                      }

                      return 0;
                  }));

    assert.commandFailed(mydb.runCommand({listCollections: 1}));
    assert.commandFailed(mydb.runCommand({listCollections: 1, filter: {type: 'view'}}));

    // Fix database state for end of test validation and burn-in tests
    mydb.dropDatabase();
})();
