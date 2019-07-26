// Test the $text query operator.
// @tags: [requires_non_retryable_writes]
(function() {
"use strict";

const coll = db.getCollection("fts_querylang");
coll.drop();

assert.commandWorked(coll.insert({_id: 0, unindexedField: 0, a: "textual content"}));
assert.commandWorked(coll.insert({_id: 1, unindexedField: 1, a: "additional content"}));
assert.commandWorked(coll.insert({_id: 2, unindexedField: 2, a: "irrelevant content"}));
assert.commandWorked(coll.createIndex({a: "text"}));

// Test text query with no results.
assert.eq(false, coll.find({$text: {$search: "words"}}).hasNext());

// Test basic text query.
let results = coll.find({$text: {$search: "textual content -irrelevant"}}).toArray();
assert.eq(results.length, 2, results);
assert.neq(results[0]._id, 2, results);
assert.neq(results[1]._id, 2, results);

// Test sort with basic text query.
results = coll.find({$text: {$search: "textual content -irrelevant"}})
              .sort({unindexedField: 1})
              .toArray();
assert.eq(results.length, 2, results);
assert.eq(results[0]._id, 0, results);
assert.eq(results[1]._id, 1, results);

// Test skip with basic text query.
results = coll.find({$text: {$search: "textual content -irrelevant"}})
              .sort({unindexedField: 1})
              .skip(1)
              .toArray();
assert.eq(results.length, 1, results);
assert.eq(results[0]._id, 1, results);

// Test limit with basic text query.
results = coll.find({$text: {$search: "textual content -irrelevant"}})
              .sort({unindexedField: 1})
              .limit(1)
              .toArray();
assert.eq(results.length, 1, results);
assert.eq(results[0]._id, 0, results);

// Test $and of basic text query with indexed expression.
results = coll.find({$text: {$search: "content -irrelevant"}, _id: 1}).toArray();
assert.eq(results.length, 1, results);
assert.eq(results[0]._id, 1, results);

// Test $and of basic text query with indexed expression and bad language.
assert.commandFailedWithCode(
    assert.throws(function() {
                     coll.find({
                             $text: {$search: "content -irrelevant", $language: "spanglish"},
                             _id: 1
                         })
                         .itcount();
                 }),
                 ErrorCodes.BadValue);

// Test $and of basic text query with unindexed expression.
results = coll.find({$text: {$search: "content -irrelevant"}, unindexedField: 1}).toArray();
assert.eq(results.length, 1, results);
assert.eq(results[0]._id, 1, results);

// Test $language.
let cursor = coll.find({$text: {$search: "contents", $language: "none"}});
assert.eq(false, cursor.hasNext());

cursor = coll.find({$text: {$search: "contents", $language: "EN"}});
assert.eq(true, cursor.hasNext());

cursor = coll.find({$text: {$search: "contents", $language: "spanglish"}});
assert.commandFailedWithCode(assert.throws(function() {
                                              cursor.next();
                                          }),
                                          ErrorCodes.BadValue);

// Test update with $text.
coll.update({$text: {$search: "textual content -irrelevant"}}, {$set: {b: 1}}, {multi: true});
assert.eq(2, coll.find({b: 1}).itcount(), 'incorrect number of documents updated');

// $text cannot be contained within a $nor.
assert.commandFailedWithCode(
    assert.throws(function() {
                     coll.find({$nor: [{$text: {$search: 'a'}}]}).itcount();
                 }),
                 ErrorCodes.BadValue);
}());
