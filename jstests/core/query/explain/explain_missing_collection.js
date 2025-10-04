/**
 * Test explaining various operations against a non-existent collection.
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # explain is a non-retryable command
 *   requires_non_retryable_commands,
 * ]
 */
// Ensure db exists (needed for explain to work).
db.filler_collection.drop();
assert.commandWorked(db.createCollection("filler_collection"));
db.filler_collection.drop();

let missingColl = db.explain_null_collection;

let explain;
let explainColl;

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
