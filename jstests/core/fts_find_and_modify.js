/**
 * Test that findAndModify works with $text search predicates.
 *
 * @tags: [
 *   # Cannot run when collections are implicitly sharded, since findAndModify requires the query
 *   # predicate to contain the shard key.
 *   assumes_unsharded_collection,
 *   # We chose not to backport the bug fix for $text + findAndModify to the 4.4 branch, so all
 *   # nodes must be at least binary version 4.7.
 *   requires_fcv_46,
 *   # Ban in any configurations that require retryable writes. Although findAndModify is a
 *   # retryable write command, the 'fields' option does not currently work with retryable writes.
 *   # See SERVER-31242.
 *   requires_non_retryable_writes,
 * ]
 */
(function() {
"use strict";

const coll = db.fts_find_and_modify;
coll.drop();

assert.commandWorked(coll.createIndex({numbers: "text"}));
assert.commandWorked(coll.insert([
    {_id: 1, numbers: "one"},
    {_id: 2, numbers: "two"},
    {_id: 3, numbers: "one two"},
    {_id: 4, numbers: "four"}
]));

// Test that findAndModify can delete a document matching a text search predicate.
assert.eq({_id: 4, numbers: "four"},
          coll.findAndModify({query: {$text: {$search: "four"}}, remove: true}));
assert.commandWorked(coll.insert({_id: 4, numbers: "four"}));

// Test that findAndModify can update a document matching a text search predicate, and return the
// old version of the document.
assert.eq({_id: 4, numbers: "four"},
          coll.findAndModify(
              {query: {$text: {$search: "four"}}, update: [{$set: {addedField: 1}}], new: false}));
assert.eq({_id: 4, numbers: "four", addedField: 1}, coll.findOne({_id: 4}));

// Test that findAndModify can update a document matching a text search predicate, and return the
// new version of the document.
assert.eq({_id: 4, numbers: "four", addedField: 2}, coll.findAndModify({
    query: {$text: {$search: "four"}},
    update: [{$set: {addedField: {$add: ["$addedField", 1]}}}],
    new: true
}));
assert.eq({_id: 4, numbers: "four", addedField: 2}, coll.findOne({_id: 4}));

// Test that findAndModify can delete a document and project its text score.
assert.eq(
    {_id: 4, numbers: "four", addedField: 2, score: 1.1},
    coll.findAndModify(
        {query: {$text: {$search: "four"}}, fields: {score: {$meta: "textScore"}}, remove: true}));
assert.commandWorked(coll.insert({_id: 4, numbers: "four"}));

// Test that findAndModify can delete a document, where the document is chosen sorting by text
// score.
assert.eq(
    {_id: 3, numbers: "one two"},
    coll.findAndModify(
        {query: {$text: {$search: "one two"}}, sort: {score: {$meta: "textScore"}}, remove: true}));
assert.commandWorked(coll.insert({_id: 3, numbers: "one two"}));

// Test that findAndModify can delete a document and both sort and project the text score.
assert.eq({_id: 3, numbers: "one two", score: 1.5}, coll.findAndModify({
    query: {$text: {$search: "one two"}},
    fields: {score: {$meta: "textScore"}},
    sort: {score: {$meta: "textScore"}},
    remove: true
}));
assert.commandWorked(coll.insert({_id: 3, numbers: "one two"}));

// Test that findAndModify can update a document, returning the old document with the text score
// projected.
assert.eq({_id: 4, numbers: "four", score: 1.1}, coll.findAndModify({
    query: {$text: {$search: "four"}},
    update: [{$set: {addedField: 1}}],
    fields: {score: {$meta: "textScore"}},
    new: false
}));

// Test that findAndModify can update a document, returning the new document with the text score
// projected.
assert.eq({_id: 4, numbers: "four", addedField: 2, score: 1.1}, coll.findAndModify({
    query: {$text: {$search: "four"}},
    update: [{$set: {addedField: {$add: ["$addedField", 1]}}}],
    fields: {score: {$meta: "textScore"}},
    new: true
}));

// Test that findAndModify can update a document chosen by a "textScore" $meta-sort, and return the
// old version of the document.
assert.eq({_id: 3, numbers: "one two"}, coll.findAndModify({
    query: {$text: {$search: "one two"}},
    update: [{$set: {addedField: 1}}],
    sort: {score: {$meta: "textScore"}},
    new: false
}));

// Test that findAndModify can update a document chosen by a "textScore" $meta-sort, and return the
// new version of the document.
assert.eq({_id: 3, numbers: "one two", addedField: 2}, coll.findAndModify({
    query: {$text: {$search: "one two"}},
    update: [{$set: {addedField: {$add: ["$addedField", 1]}}}],
    sort: {score: {$meta: "textScore"}},
    new: true
}));

// Test that findAndModify can update a document chosen by a "textScore" $meta-sort, and return the
// old version of the document with the text score projected.
assert.eq({_id: 3, numbers: "one two", addedField: 2, score: 1.5}, coll.findAndModify({
    query: {$text: {$search: "one two"}},
    update: [{$set: {addedField: {$add: ["$addedField", 1]}}}],
    fields: {score: {$meta: "textScore"}},
    sort: {score: {$meta: "textScore"}},
    new: false
}));

// Test that findAndModify can update a document chosen by a "textScore" $meta-sort, and return the
// new version of the document with the text score projected.
assert.eq({_id: 3, numbers: "one two", addedField: 4, score: 1.5}, coll.findAndModify({
    query: {$text: {$search: "one two"}},
    update: [{$set: {addedField: {$add: ["$addedField", 1]}}}],
    fields: {score: {$meta: "textScore"}},
    sort: {score: {$meta: "textScore"}},
    new: true
}));
}());
