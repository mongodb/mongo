// Test sorting with text score metadata.
// @tags: [
//   sbe_incompatible,
// ]
(function() {
"use strict";

const kUnavailableMetadataErrCode = 40218;

const coll = db.getSiblingDB("test").getCollection("fts_score_sort");
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: "textual content"}));
assert.commandWorked(coll.insert({_id: 1, a: "additional content"}));
assert.commandWorked(coll.insert({_id: 2, a: "irrelevant content"}));
assert.commandWorked(coll.createIndex({a: "text"}));

// $meta sort specification should be rejected if it has additional keys.
assert.throws(function() {
    coll.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
        .sort({score: {$meta: "textScore", extra: 1}})
        .itcount();
});

// $meta sort specification should be rejected if the type of meta sort is not known.
assert.throws(function() {
    coll.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
        .sort({score: {$meta: "unknown"}})
        .itcount();
});

// Sort spefication should be rejected if a $-keyword other than $meta is used.
assert.throws(function() {
    coll.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
        .sort({score: {$notMeta: "textScore"}})
        .itcount();
});

// Sort spefication should be rejected if it is a string, not an object with $meta.
assert.throws(function() {
    coll.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
        .sort({score: "textScore"})
        .itcount();
});

// Sort by the text score.
let results =
    coll.find({$text: {$search: "textual content -irrelevant"}}, {score: {$meta: "textScore"}})
        .sort({score: {$meta: "textScore"}})
        .toArray();
assert.eq(
    results,
    [{_id: 0, a: "textual content", score: 1.5}, {_id: 1, a: "additional content", score: 0.75}]);

// Sort by {_id descending, score} and verify the order is right.
results =
    coll.find({$text: {$search: "textual content -irrelevant"}}, {score: {$meta: "textScore"}})
        .sort({_id: -1, score: {$meta: "textScore"}})
        .toArray();
assert.eq(
    results,
    [{_id: 1, a: "additional content", score: 0.75}, {_id: 0, a: "textual content", score: 1.5}]);

// Can $meta sort by text score without a meta projection using either find or agg.
let expectedResults = [{_id: 0, a: "textual content"}, {_id: 1, a: "additional content"}];
results = coll.find({$text: {$search: "textual content -irrelevant"}})
              .sort({score: {$meta: "textScore"}})
              .toArray();
assert.eq(results, expectedResults);
results = coll.aggregate([
                  {$match: {$text: {$search: "textual content -irrelevant"}}},
                  {$sort: {score: {$meta: "textScore"}}}
              ])
              .toArray();
assert.eq(results, expectedResults);

// $meta-sort by text score fails if there is no $text predicate, for both find and agg.
let error = assert.throws(() => coll.find().sort({score: {$meta: "textScore"}}).itcount());
assert.commandFailedWithCode(error, kUnavailableMetadataErrCode);
error = assert.throws(() => coll.aggregate([{$sort: {score: {$meta: "textScore"}}}]).itcount());
assert.commandFailedWithCode(error, kUnavailableMetadataErrCode);

// Test a sort pattern like {<field>: {$meta: "textScore"}} is legal even if <field> is explicitly
// included by the projection. Test that this is true for both the find and aggregate commands.
expectedResults = [{a: "textual content"}, {a: "additional content"}];
results = coll.find({$text: {$search: "textual content -irrelevant"}}, {_id: 0, a: 1})
              .sort({a: {$meta: "textScore"}})
              .toArray();
assert.eq(results, expectedResults);
results = coll.aggregate([
                  {$match: {$text: {$search: "textual content -irrelevant"}}},
                  {$project: {_id: 0, a: 1}},
                  {$sort: {a: {$meta: "textScore"}}}
              ])
              .toArray();
assert.eq(results, expectedResults);

// Test that both find and agg can $meta-project the textScore with a non-$meta sort on the same
// field. The semantics of find are that the sort logically occurs before the projection, so we
// expect the data to be sorted according the values that were present prior to the $meta
// projection.
expectedResults = [{_id: 0, a: 0.75}, {_id: 1, a: 1.5}];
results = coll.find({$text: {$search: "additional content -irrelevant"}},
                    {_id: 1, a: {$meta: "textScore"}})
              .sort({a: -1})
              .toArray();
assert.eq(results, expectedResults);
results = coll.aggregate([
                  {$match: {$text: {$search: "additional content -irrelevant"}}},
                  {$sort: {a: -1}},
                  {$project: {_id: 1, a: {$meta: "textScore"}}}
              ])
              .toArray();
assert.eq(results, expectedResults);

// Test that an aggregate command with a $project-then-$sort pipeline can sort on the
// $meta-projected data without repeating the $meta operator in the $sort.
results = coll.aggregate([
                  {$match: {$text: {$search: "textual content -irrelevant"}}},
                  {$project: {a: 1, score: {$meta: "textScore"}}},
                  {$sort: {score: -1}}
              ])
              .toArray();
assert.eq(
    results,
    [{_id: 0, a: "textual content", score: 1.5}, {_id: 1, a: "additional content", score: 0.75}]);
}());
