// Operators that use a random generator ($sample and $rand) are not allowed to be cached
// as part of a non-correlated prefix.
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isSharded.

const coll = db.getCollection('lookup_random');
coll.drop();
assert.commandWorked(coll.insert(Array.from({length: 200}, (_, i) => ({_id: i}))));

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(coll) && !isShardedLookupEnabled) {
    return;
}

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
