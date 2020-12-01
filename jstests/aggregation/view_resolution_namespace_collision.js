/**
 * Tests that view resolution works correctly when a cross-database aggregation targets a
 * collection and a view with the same name on different databases.
 *
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
(function() {
"use strict";

const dbName = "test";
const testDB = db.getSiblingDB(dbName);
const siblingDBName = "otherDB";
const siblingDB = testDB.getSiblingDB(siblingDBName);

// Since cross-db $merge only works against existing databases in a sharded environment, we create a
// dummy collection on 'siblingDB' to allow this test to run in a sharded environment.
assert.commandWorked(siblingDB.foo.insert({}));

const sourceName = jsTestName();
const otherCollName = jsTestName() + "_other_coll";
const collidingName = "collision_collection";
const sourceColl = testDB[sourceName];
const otherColl = testDB[otherCollName];

sourceColl.drop();
otherColl.drop();
testDB[collidingName].drop();

assert.commandWorked(sourceColl.insert({_id: 0}));
assert.commandWorked(otherColl.insert({_id: 0, notsecret: 1, secret: "really secret"}));

// Create a view on 'testDB' that will have the same name as the collection that $merge will create.
assert.commandWorked(testDB.runCommand(
    {create: collidingName, viewOn: otherCollName, pipeline: [{$project: {secret: 0}}]}));

// Verify that the view gets resolved correctly when performing a cross database aggregation where
// the $lookup references a view on the source database and the $merge references a collection on
// the target database with the same name as the view.
const lookupStage = {
    $lookup: {from: collidingName, localField: "_id", foreignField: "_id", as: "matches"}
};
const withoutWrite = sourceColl.aggregate([lookupStage]).toArray();
const mergeStage = {
    $merge: {into: {db: siblingDBName, coll: collidingName}}
};

// The aggregate should always use the view on 'testDB', not an empty collection on 'siblingDB'.
sourceColl.aggregate([lookupStage, mergeStage]).toArray();
const withWrite = siblingDB[collidingName].find().toArray();
assert.eq(withoutWrite, withWrite);
siblingDB[collidingName].drop();

siblingDB.dropDatabase();
})();