(function() {
"use strict";

const dbname = 'views_invalid';
let invalidDB = db.getSiblingDB(dbname);

invalidDB.system.views.drop();
assert.commandWorked(invalidDB.createCollection("system.views"));

// Create a database with one valid and one invalid view through direct system.views writes.
assert.commandWorked(invalidDB.coll.insert({x: 1}));
assert.commandWorked(invalidDB.adminCommand({
    applyOps: [{
        op: "i",
        ns: dbname + ".system.views",
        o: {_id: dbname + '.view', viewOn: 'coll', pipeline: []}
    }]
}));
assert.eq(invalidDB.view.findOne({}, {_id: 0}),
          {x: 1},
          'find on view created with direct write to views catalog should work');
assert.commandWorked(invalidDB.adminCommand(
    {applyOps: [{op: "i", ns: dbname + ".system.views", o: {_id: "invalid", pipeline: 3.0}}]}));

// Check that view-related commands fail with an invalid view catalog, but other commands on
// existing collections still succeed.
assert.commandFailedWithCode(invalidDB.runCommand({find: 'view'}),
                             ErrorCodes.InvalidViewDefinition,
                             'find on existing view in DB with invalid system.views should fail');

assert.eq(invalidDB.coll.findOne({}, {_id: 0}),
          {x: 1},
          'find on existing collection in DB with invalid views catalog should work');

assert.commandWorked(invalidDB.coll.insert({x: 2}),
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

assert.commandFailedWithCode(invalidDB.createCollection('x'),
                             ErrorCodes.InvalidViewDefinition,
                             'creating a collection in DB with invalid views catalog should fail');

assert.commandFailedWithCode(
    invalidDB.runCommand({find: 'x'}),
    ErrorCodes.InvalidViewDefinition,
    'find on non-existent collection in DB with invalid system.views should fail');

// Now fix the database by removing the invalid system.views entry, and check all is OK.
assert.commandWorked(
    invalidDB.adminCommand(
        {applyOps: [{op: "d", ns: dbname + ".system.views", o: {_id: "invalid"}}]}),
    "should be able to remove invalid view with direct write to view catalog");
assert.commandWorked(
    invalidDB.coll.insert({x: 1}),
    'after remove invalid view from catalog, should be able to create new collection');
assert.eq(invalidDB.view.findOne({}, {_id: 0}),
          {x: 1},
          'find on view should work again after removing invalid view from catalog');
})();
