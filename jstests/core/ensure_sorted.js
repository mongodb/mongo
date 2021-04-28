// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
// @tags: [
//   assumes_unsharded_collection,
//   requires_getmore,
// ]

// SERVER-17011 Tests whether queries which specify sort and batch size can generate results out of
// order due to the ntoreturn hack. The EnsureSortedStage should solve this problem.
(function() {
'use strict';
const collName = "ensure_sorted";
const coll = db[collName];
const kDocList =
    [{_id: 0, a: 1, b: 4}, {_id: 1, a: 2, b: 3}, {_id: 2, a: 3, b: 2}, {_id: 3, a: 4, b: 1}];
const kBatchSize = 2;
const kFilters = [
    {a: {$lt: 5}},

    // Optimized multi interval index bounds (the system knows which intervals need to be scanned).
    {a: {$in: [1, 2, 3, 4]}, b: {$gt: 0, $lt: 5}},
    {$or: [{a: {$in: [1, 2]}, b: {$gte: 3, $lt: 5}}, {a: {$in: [3, 4]}, b: {$gt: 0, $lt: 3}}]},

    // Generic multi interval index bounds (index intervals unknown prior to query runtime).
    {a: {$gt: 0}, b: {$lt: 5}},
    {$or: [{a: {$gte: 0}, b: {$gte: 3}}, {a: {$gte: 0}, b: {$lte: 2}}]}
];

for (const filter of kFilters) {
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.insert(kDocList));

    const cursor = coll.find(filter).sort({a: 1}).batchSize(kBatchSize);
    assert.eq(cursor.next(), {_id: 0, a: 1, b: 4});
    assert.eq(cursor.next(), {_id: 1, a: 2, b: 3});

    assert.commandWorked(coll.update({b: 2}, {$set: {a: 10}}));
    let result = cursor.next();

    // We might either drop the document where "b" is 2 from the result set, or we might include the
    // old version of this document (before the update is applied). Either is acceptable, but
    // out-of-order results are unacceptable.
    assert(result.b === 2 || result.b === 1,
           "cursor returned: " + printjson(result) + " for filter: " + printjson(filter));
}
})();
