const dbname = "views_invalid";
let invalidDB = db.getSiblingDB(dbname);

invalidDB.system.views.drop();
assert.commandWorked(invalidDB.createCollection("system.views"));

// Create a database with one valid and one invalid view through direct system.views writes.
assert.commandWorked(invalidDB.coll.insert({x: 1}));
assert.commandWorked(
    invalidDB.adminCommand({
        applyOps: [
            {
                op: "i",
                ns: dbname + ".system.views",
                o: {_id: dbname + ".view", viewOn: "coll", pipeline: []},
            },
        ],
    }),
);
assert.eq(
    invalidDB.view.findOne({}, {_id: 0}),
    {x: 1},
    "find on view created with direct write to views catalog should work",
);
assert.commandWorked(
    invalidDB.adminCommand({applyOps: [{op: "i", ns: dbname + ".system.views", o: {_id: "invalid", pipeline: 3.0}}]}),
);

// Make sure we logged an error message about the invalid view.
assert(checkLog.checkContainsOnceJson(invalidDB, 20326));

// Check that operations on valid views work, even with an invalid view in the catalog.
assert.eq(
    invalidDB.view.findOne({}, {_id: 0}),
    {x: 1},
    "find on valid existing view in DB with invalid system.views should still work",
);

assert.eq(
    invalidDB.coll.findOne({}, {_id: 0}),
    {x: 1},
    "find on existing collection in DB with invalid views catalog should work",
);

assert.commandWorked(
    invalidDB.coll.insert({x: 2}),
    "insert in existing collection in DB with invalid views catalog should work",
);

assert.commandWorked(
    invalidDB.x.insert({x: 2}),
    "insert into new collection in DB with invalid views catalog should work",
);

assert.commandWorked(
    invalidDB.runCommand({drop: "coll"}),
    "dropping an existing collection in DB with invalid views catalog should work",
);

assert.commandFailedWithCode(
    invalidDB.runCommand({drop: "view"}),
    ErrorCodes.InvalidViewDefinition,
    "dropping an existing view in DB with invalid views catalog should fail",
);

assert.commandWorked(
    invalidDB.createCollection("coll2"),
    "creating a collection in DB with invalid views catalog should work",
);

assert.commandFailedWithCode(
    invalidDB.createCollection("view2", {viewOn: "coll"}),
    ErrorCodes.InvalidViewDefinition,
    "creating a view in DB with invalid views catalog should fail",
);

assert.commandWorked(
    invalidDB.runCommand({find: "nonexistent"}),
    "find on non-existent collection in DB with invalid system.views should work",
);

// Now fix the database by removing the invalid system.views entry, and check all is OK.
assert.commandWorked(
    invalidDB.adminCommand({applyOps: [{op: "d", ns: dbname + ".system.views", o: {_id: "invalid"}}]}),
    "should be able to remove invalid view with direct write to view catalog",
);

assert.commandWorked(
    invalidDB.createCollection("view2", {viewOn: "coll"}),
    "after removing invalid view from catalog, should be able to create new view",
);

assert.eq(true, invalidDB.view2.drop(), "after removing invalid view from catalog, should be able to drop any view");
