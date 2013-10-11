// Test $text query operator.

var t = db.getSiblingDB("test").getCollection("fts_querylang");
var cursor;
var results;

db.adminCommand({setParameter:1, textSearchEnabled:true});
db.adminCommand({setParameter:1, newQueryFrameworkEnabled:true});

t.drop();

t.insert({_id:0, _idCopy: 0, a:"textual content"});
t.insert({_id:1, _idCopy: 1, a:"additional content"});
t.insert({_id:2, _idCopy: 2, a:"irrelevant content"});
t.ensureIndex({a:"text"});

// Test implicit sort for basic text query.
results = t.find({$text: {$search: "textual content -irrelevant"}}).toArray();
assert.eq(results.length, 2);
assert.eq(results[0]._id, 0);
assert.eq(results[1]._id, 1);

// Test implicit limit for basic text query.
for (var i=0; i<200; i++) {
    t.insert({a: "temporary content"})
}
results = t.find({$text: {$search: "textual content -irrelevant"}}).toArray();
assert.eq(results.length, 100);
t.remove({a: "temporary content"});

// Test skip for basic text query.
results = t.find({$text: {$search: "textual content -irrelevant"}}).skip(1).toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 1);

// Test explicit limit for basic text query.
results = t.find({$text: {$search: "textual content -irrelevant"}}).limit(1).toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 0);

// TODO Test basic text query with explicit sort, once sort is enabled in the new query framework.

// TODO Test basic text query with projection, once projection is enabled in the new query
// framework.

// Test $and of basic text query with indexed expression.
results = t.find({$text: {$search: "content -irrelevant"},
                  _id: 1}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 1);

// Test $and of basic text query with unindexed expression.
results = t.find({$text: {$search: "content -irrelevant"},
                  _idCopy: 1}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 1);

// TODO Test that $or of basic text query with indexed expression is disallowed.

// Test that $or of basic text query with unindexed expression is disallowed.
assert.throws(function() { t.find({$or: [{$text: {$search: "content -irrelevant"}},
                                         {_idCopy: 2}]}).itcount(); });

// TODO Test invalid inputs for $text, $search, $language.

// TODO Test $language.

// TODO Test $and of basic text query with geo expression.

// TODO Test update with $text, once it is enabled with the new query framework.

// TODO Test remove with $text, once it is enabled with the new query framework.

// TODO Test count with $text, once it is enabled with the new query framework.

// TODO Test findAndModify with $text, once it is enabled with the new query framework.

// TODO Test aggregate with $text, once it is enabled with the new query framework.

// TODO Test that old query framework rejects $text queries.

// TODO Test that $text fails without a text index.

// TODO Test that $text accepts a hint of the text index.

// TODO Test that $text fails if a different index is hinted.

// TODO Test $text with {$natural:1} sort, {$natural:1} hint.
