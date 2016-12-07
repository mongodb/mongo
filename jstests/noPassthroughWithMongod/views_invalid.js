(function() {
    "use strict";

    const dbname = 'views_invalid';
    let invalidDB = db.getSiblingDB(dbname);

    // Wait for the invalid view definition to be replicated to any secondaries and then drop the
    // database.
    assert.writeOK(invalidDB.system.views.insert({z: '\0\uFFFFf'}),
                   {writeConcern: {w: "majority"}});
    invalidDB.dropDatabase();

    // Create a database with one valid and one invalid view through direct system.views writes.
    assert.writeOK(invalidDB.coll.insert({x: 1}));
    assert.writeOK(
        invalidDB.system.views.insert({_id: dbname + '.view', viewOn: 'coll', pipeline: []}));
    assert.eq(invalidDB.view.findOne({}, {_id: 0}),
              {x: 1},
              'find on view created with direct write to views catalog should work');
    assert.writeOK(invalidDB.system.views.insert({_id: 'invalid', pipeline: 3.0}));

    // Check that view-related commands fail with an invalid view catalog, but other commands on
    // existing collections still succeed.
    assert.commandFailedWithCode(
        invalidDB.runCommand({find: 'view'}),
        ErrorCodes.InvalidViewDefinition,
        'find on existing view in DB with invalid system.views should fail');

    assert.eq(invalidDB.coll.findOne({}, {_id: 0}),
              {x: 1},
              'find on existing collection in DB with invalid views catalog should work');

    assert.writeOK(invalidDB.coll.insert({x: 2}),
                   'insert in existing collection in DB with invalid views catalog should work');

    assert.writeError(invalidDB.x.insert({x: 2}),
                      'insert into new collection in DB with invalid views catalog should fail');

    assert.commandWorked(
        invalidDB.runCommand({drop: 'coll'}),
        'dropping an existing collection in DB with invalid views catalog should work');

    assert.commandFailedWithCode(
        invalidDB.runCommand({drop: 'view'}),
        ErrorCodes.InvalidViewDefinition,
        'dropping an existing view in DB with invalid views catalog should fail');

    assert.commandFailedWithCode(
        invalidDB.createCollection('x'),
        ErrorCodes.InvalidViewDefinition,
        'creating a collection in DB with invalid views catalog should fail');

    assert.commandFailedWithCode(
        invalidDB.runCommand({find: 'x'}),
        ErrorCodes.InvalidViewDefinition,
        'find on non-existent collection in DB with invalid system.views should fail');

    // Now fix the database by removing the invalid system.views entry, and check all is OK.
    assert.writeOK(invalidDB.system.views.remove({_id: 'invalid'}),
                   'should be able to remove invalid view with direct write to view catalog');
    assert.writeOK(
        invalidDB.coll.insert({x: 1}),
        'after remove invalid view from catalog, should be able to create new collection');
    assert.eq(invalidDB.view.findOne({}, {_id: 0}),
              {x: 1},
              'find on view should work again after removing invalid view from catalog');
})();
