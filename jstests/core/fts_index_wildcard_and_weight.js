// Test that on a text index that matches all fields does not use a weight from a named field.
// This test was designed to reproduce SERVER-45363.
//
// @tags: [
//   sbe_incompatible,
// ]
(function() {
"use strict";
var coll = db.getCollection(jsTestName());
coll.drop();

assert.commandWorked(coll.createIndex(
    {"$**": "text"}, {name: "fullTextIndex", weights: {name: 500}, default_language: "english"}));
assert.commandWorked(coll.insert({name: 'Spot', guardian: 'Kevin'}));
assert.commandWorked(coll.insert({name: 'Kevin', guardian: 'Spot'}));
var results = coll.aggregate([
                      {$match: {$text: {$search: "Kevin"}}},
                      {$sort: {score: {$meta: "textScore"}}},
                      {$project: {name: 1, score: {$meta: "textScore"}}}
                  ])
                  .toArray();
assert.gt(results[0].score, results[1].score);
assert.eq(results[0].name, "Kevin");

coll.drop();
assert.commandWorked(coll.createIndex(
    {"$**": "text"},
    {name: "fullTextIndex", weights: {name: 500, tag: 250}, default_language: "english"}));
assert.commandWorked(coll.insert({name: 'Spot', guardian: 'Kevin', special: 'Dog', tag: 'Nice'}));
assert.commandWorked(coll.insert({name: 'Kevin', guardian: 'Spot', special: 'Human', tag: 'Mean'}));
assert.commandWorked(
    coll.insert({name: 'Whiskers', guardian: 'Carl', special: 'Cat', tag: 'Kevin'}));
assert.commandWorked(
    coll.insert({name: 'McFlufferson', guardian: 'Steve', special: 'Kevin', tag: 'Fluffy'}));

results = coll.aggregate([
                  {$match: {$text: {$search: "Kevin"}}},
                  {$sort: {score: {$meta: "textScore"}}},
                  {$project: {name: 1, score: {$meta: "textScore"}}}
              ])
              .toArray();
assert.eq(results[0].name, "Kevin", results);
assert.eq(results[1].name, "Whiskers", results);
assert.gt(results[0].score, results[1].score, results);
assert.eq(results[2].score, results[3].score, results);
})();
