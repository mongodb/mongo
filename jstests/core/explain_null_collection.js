// Test explain of various operations against non-existent databases and collections.

var explain;
var explainColl;

//
// Part 1: Non-existent database.
//

var explainMissingDb = db.getSiblingDB("explainMissingDb");

// .find()
explainMissingDb.dropDatabase();
explain = explainMissingDb.collection.explain("executionStats").find().finish();
assert.commandWorked(explain);
assert("executionStats" in explain);

// .count()
explainMissingDb.dropDatabase();
explain = explainMissingDb.collection.explain("executionStats").count();
assert.commandWorked(explain);
assert("executionStats" in explain);

// .group()
explainMissingDb.dropDatabase();
explainColl = explainMissingDb.collection.explain("executionStats");
explain = explainColl.group({key: "a", initial: {}, reduce: function() { } });
assert.commandWorked(explain);
assert("executionStats" in explain);

// .remove()
explainMissingDb.dropDatabase();
explain = explainMissingDb.collection.explain("executionStats").remove({a: 1});
assert.commandWorked(explain);
assert("executionStats" in explain);

// .update() with upsert: false
explainMissingDb.dropDatabase();
explainColl = explainMissingDb.collection.explain("executionStats");
explain = explainColl.update({a: 1}, {b: 1});
assert.commandWorked(explain);
assert("executionStats" in explain);

// .update() with upsert: true
explainMissingDb.dropDatabase();
explainColl = explainMissingDb.collection.explain("executionStats");
explain = explainColl.update({a: 1}, {b: 1}, {upsert: true});
assert.commandWorked(explain);
assert("executionStats" in explain);

// .aggregate()
explainMissingDb.dropDatabase();
explain = explainMissingDb.collection.explain("executionStats").aggregate([{$match: {a: 1}}]);
assert.commandWorked(explain);

//
// Part 2: Non-existent collection.
//

var missingColl = db.explain_null_collection;

// .find()
missingColl.drop();
explain = missingColl.explain("executionStats").find().finish();
assert.commandWorked(explain);
assert("executionStats" in explain);

// .count()
missingColl.drop();
explain = missingColl.explain("executionStats").count();
assert.commandWorked(explain);
assert("executionStats" in explain);

// .group()
missingColl.drop();
explainColl = missingColl.explain("executionStats");
explain = explainColl.group({key: "a", initial: {}, reduce: function() { } });
assert.commandWorked(explain);
assert("executionStats" in explain);

// .remove()
missingColl.drop();
explain = missingColl.explain("executionStats").remove({a: 1});
assert.commandWorked(explain);
assert("executionStats" in explain);

// .update() with upsert: false
missingColl.drop();
explainColl = missingColl.explain("executionStats");
explain = explainColl.update({a: 1}, {b: 1});
assert.commandWorked(explain);
assert("executionStats" in explain);

// .update() with upsert: true
missingColl.drop();
explainColl = missingColl.explain("executionStats");
explain = explainColl.update({a: 1}, {b: 1}, {upsert: true});
assert.commandWorked(explain);
assert("executionStats" in explain);

// .aggregate()
missingColl.drop();
explain = missingColl.explain("executionStats").aggregate([{$match: {a: 1}}]);
assert.commandWorked(explain);
