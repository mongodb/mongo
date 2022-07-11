// Tests that given a $text stage before a $lookup stage, the $lookup's subpipeline cannot
// reference the text score metadata from that $text search.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For "assertErrorCode".

const outer = db.outer;
const inner = db.inner;

outer.drop();
inner.drop();

const kNoTextScoreAvailableErrCode = 40218;

// This pipeline is never legal, because the subpipeline projects out a textScore but does not
// begin with a $text search.
let pipeline = [
    {$match: {$text: {$search: "foo"}}},
    {$lookup: {from: "inner", as: "as", pipeline: [{$project: {score: {$meta: "textScore"}}}]}}
];

assert.commandWorked(outer.insert({_id: 100, a: "foo"}));
assert.commandWorked(inner.insert({_id: 100, a: "bar apple banana"}));

// Neither 'outer' nor 'inner' have  text indexes.
assertErrorCode(outer, pipeline, ErrorCodes.IndexNotFound);

// Only 'outer' has a text index.
assert.commandWorked(outer.createIndex({a: "text"}, {name: "outer_first_index"}));
assertErrorCode(outer, pipeline, kNoTextScoreAvailableErrCode);

// Only 'inner' has a text index.
assert.commandWorked(outer.dropIndex("outer_first_index"));
assert.commandWorked(inner.createIndex({a: "text"}));
assertErrorCode(outer, pipeline, ErrorCodes.IndexNotFound);

// Both 'outer' and 'inner' have a text index.
assert.commandWorked(outer.createIndex({a: "text"}));
assertErrorCode(outer, pipeline, kNoTextScoreAvailableErrCode);

// A pipeline with two text searches, one within a $lookup, will work.
pipeline = [
        {$match: {$text: {$search: "foo"}}},
        {
          $lookup: {
              from: "inner",
              as: "as",
              pipeline: [
                  {$match: {$text: {$search: "bar apple banana hello"}}},
                  {$project: {score: {$meta: "textScore"}}}
              ]
          }
        }
    ];

let expected = [{"_id": 100, "a": "foo", "as": [{"_id": 100, "score": 2}]}];
assert.eq(outer.aggregate(pipeline).toArray(), expected);

// A lookup with a text search in the subpipeline will correctly perform that search on 'from'.
pipeline = [{
        $lookup: {
            from: "inner",
            as: "as",
            pipeline: [{$match: {$text: {$search: "bar apple banana hello"}}}]
        }
    }];
expected = [{"_id": 100, "a": "foo", "as": [{"_id": 100, "a": "bar apple banana"}]}];

assert.eq(outer.aggregate(pipeline).toArray(), expected);

// A lookup with two text searches and two text score $projects will have the text scores
// reference the relevant text search.
pipeline = [
        {$match: {$text: {$search: "foo"}}},
        {
          $lookup: {
              from: "inner",
              as: "as",
              pipeline: [
                  {$match: {$text: {$search: "bar apple banana hello"}}},
                  {$project: {score: {$meta: "textScore"}}}
              ]
          }
        },
        {$project: {score: {$meta: "textScore"}, as: 1}},
    ];

expected = [{"_id": 100, "as": [{"_id": 100, "score": 2}], "score": 1.1}];

assert.eq(outer.aggregate(pipeline).toArray(), expected);

// Given a $text stage in the 'from' pipeline, the outer pipeline will not be able to access
// this $text stage's text score.
pipeline = [
        {
          $lookup: {
              from: "inner",
              as: "as",
              pipeline: [{$match: {$text: {$search: "bar apple banana hello"}}}]
          }
        },
        {$project: {score: {$meta: "textScore"}, as: 1}},
    ];

assertErrorCode(outer, pipeline, kNoTextScoreAvailableErrCode);
}());
