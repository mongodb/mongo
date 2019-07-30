// Test that the $sort stage properly errors on invalid $meta.
(function() {
"use strict";

var coll = db.sort_with_metadata;
coll.drop();
assert.writeOK(coll.insert({_id: 1, text: "apple", words: 1}));
assert.writeOK(coll.insert({_id: 2, text: "banana", words: 1}));
assert.writeOK(coll.insert({_id: 3, text: "apple banana", words: 2}));
assert.writeOK(coll.insert({_id: 4, text: "cantaloupe", words: 1}));

assert.commandWorked(coll.createIndex({text: "text"}));

assert.throws(() => coll.aggregate([
    {$match: {$text: {$search: 'apple banana'}}},
    {$sort: {textScore: {$meta: 'searchScore'}}}
]));

assert.throws(() => coll.aggregate([
    {$match: {$text: {$search: 'apple banana'}}},
    {$sort: {textScore: {$meta: 'searchHighlights'}}}
]));

assert.throws(
    () => coll.aggregate(
        [{$match: {$text: {$search: 'apple banana'}}}, {$sort: {textScore: {$meta: 'unknown'}}}]));

const results = [
    {_id: 3, text: 'apple banana', words: 2},
    {_id: 2, text: 'banana', words: 1},
    {_id: 1, text: 'apple', words: 1}
];

assert.eq(results,
          coll.aggregate([
                  {$match: {$text: {$search: 'apple banana'}}},
                  {$sort: {textScore: {$meta: 'textScore'}}}
              ])
              .toArray());

assert.sameMembers(results,
                   coll.aggregate([
                           {$match: {$text: {$search: 'apple banana'}}},
                           {$sort: {textScore: {$meta: 'randVal'}}}
                       ])
                       .toArray());
})();