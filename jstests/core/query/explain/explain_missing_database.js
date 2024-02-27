/*
 * Test explain of various operations against a non-existent database
 *
 * @tags: [
 *  # Explain on non-existent database return an error when executed on mongos
 *  # TODO SERVER-18047: re-enable in sharding suites
 *  assumes_against_mongod_not_mongos,
 * ]
 */
var explainMissingDb = db.getSiblingDB("explainMissingDb");

var explain;
var explainColl;

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