// Operators that use a random generator ($sample and $rand) are not allowed to be cached
// as part of a non-correlated prefix.
(function() {
"use strict";

const coll = db.getCollection('lookup_random');
coll.drop();
assert.commandWorked(coll.insert(Array.from({length: 200}, (_, i) => ({_id: i}))));

// $sample in the inner pipeline should be rerun per outer document.
let result = coll.aggregate([
    {$lookup: {
        from: coll.getName(),
        as: 'docs',
        pipeline: [
            {$sample: {size: 1}},
        ],
    }},
    {$unwind: "$docs"},
    {$group: {_id: null, sampled: {$addToSet: "$docs._id"}}},
]).toArray();
assert.eq(result.length, 1, result);
assert.gt(result[0].sampled.length, 1, result);

// $rand in the inner pipeline should be rerun per outer document.
result = coll.aggregate([
    {$lookup: {
        from: coll.getName(),
        as: 'docs',
        pipeline: [
            {$limit: 1},
            {$set: {r: {$rand: {}}}},
        ],
    }},
    {$unwind: "$docs"},
    {$group: {_id: null, randomValues: {$addToSet: "$docs.r"}}},
]).toArray();
assert.eq(result.length, 1, result);
assert.gt(result[0].randomValues.length, 1, result);
})();
