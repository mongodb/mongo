// Test $text query operator.

var t = db.getSiblingDB("test").getCollection("fts_querylang");
var cursor;
var results;

t.drop();

t.insert({_id: 0, unindexedField: 0, a: "textual content"});
t.insert({_id: 1, unindexedField: 1, a: "additional content"});
t.insert({_id: 2, unindexedField: 2, a: "irrelevant content"});
t.ensureIndex({a: "text"});

// Test text query with no results.
assert.eq(false, t.find({$text: {$search: "words"}}).hasNext());

// Test basic text query.
results = t.find({$text: {$search: "textual content -irrelevant"}}).toArray();
assert.eq(results.length, 2);
assert.neq(results[0]._id, 2);
assert.neq(results[1]._id, 2);

// Test sort with basic text query.
results =
    t.find({$text: {$search: "textual content -irrelevant"}}).sort({unindexedField: 1}).toArray();
assert.eq(results.length, 2);
assert.eq(results[0]._id, 0);
assert.eq(results[1]._id, 1);

// Test skip with basic text query.
results = t.find({$text: {$search: "textual content -irrelevant"}})
              .sort({unindexedField: 1})
              .skip(1)
              .toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 1);

// Test limit with basic text query.
results = t.find({$text: {$search: "textual content -irrelevant"}})
              .sort({unindexedField: 1})
              .limit(1)
              .toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 0);

// TODO Test basic text query with sort, once sort is enabled in the new query framework.

// TODO Test basic text query with projection, once projection is enabled in the new query
// framework.

// Test $and of basic text query with indexed expression.
results = t.find({$text: {$search: "content -irrelevant"}, _id: 1}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 1);

// Test $and of basic text query with indexed expression, and bad language
assert.throws(function() {
    t.find({$text: {$search: "content -irrelevant", $language: "spanglish"}, _id: 1}).itcount();
});

// Test $and of basic text query with unindexed expression.
results = t.find({$text: {$search: "content -irrelevant"}, unindexedField: 1}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0]._id, 1);

// TODO Test invalid inputs for $text, $search, $language.

// Test $language.
cursor = t.find({$text: {$search: "contents", $language: "none"}});
assert.eq(false, cursor.hasNext());

cursor = t.find({$text: {$search: "contents", $language: "EN"}});
assert.eq(true, cursor.hasNext());

cursor = t.find({$text: {$search: "contents", $language: "spanglish"}});
assert.throws(function() {
    cursor.next();
});

// TODO Test $and of basic text query with geo expression.

// Test update with $text.
t.update({$text: {$search: "textual content -irrelevant"}}, {$set: {b: 1}}, {multi: true});
assert.eq(2, t.find({b: 1}).itcount(), 'incorrect number of documents updated');

// TODO Test remove with $text, once it is enabled with the new query framework.

// TODO Test count with $text, once it is enabled with the new query framework.

// TODO Test findAndModify with $text, once it is enabled with the new query framework.

// TODO Test aggregate with $text, once it is enabled with the new query framework.

// TODO Test that old query framework rejects $text queries.

// TODO Test that $text fails without a text index.

// TODO Test that $text accepts a hint of the text index.

// TODO Test that $text fails if a different index is hinted.

// TODO Test $text with {$natural:1} sort, {$natural:1} hint.
