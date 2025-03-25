// Test that the $sort stage properly errors on invalid $meta.
// This test was adjusted as we start to allow sorting by "searchScore".
// @tags: [featureFlagRankFusionFull, requires_fcv_82]

const kUnavailableMetadataErrCode = 40218;
var coll = db.sort_with_metadata;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, text: "apple", words: 1}));
assert.commandWorked(coll.insert({_id: 2, text: "banana", words: 1}));
assert.commandWorked(coll.insert({_id: 3, text: "apple banana", words: 2}));
assert.commandWorked(coll.insert({_id: 4, text: "cantaloupe", words: 1}));

assert.commandWorked(coll.createIndex({text: "text"}));

assert.throwsWithCode(() => coll.aggregate([
    {$match: {$text: {$search: 'apple banana'}}},
    {$sort: {textScore: {$meta: 'searchScore'}}}
]),
                      kUnavailableMetadataErrCode);

assert.throwsWithCode(() => coll.aggregate([
    {$match: {$text: {$search: 'apple banana'}}},
    {$set: {textScore: {$meta: 'searchScore'}}}
]),
                      kUnavailableMetadataErrCode);

assert.throwsWithCode(() => coll.aggregate([
    {$match: {$text: {$search: 'apple banana'}}},
    {$sort: {textScore: {$meta: 'vectorSearchScore'}}}
]),
                      kUnavailableMetadataErrCode);

assert.throwsWithCode(() => coll.aggregate([
    {$match: {$text: {$search: 'apple banana'}}},
    {$set: {textScore: {$meta: 'vectorSearchScore'}}}
]),
                      kUnavailableMetadataErrCode);

assert.throwsWithCode(() => coll.aggregate([
    {$match: {$text: {$search: 'apple banana'}}},
    {$sort: {textScore: {$meta: 'searchHighlights'}}}
]),
                      31138);

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
