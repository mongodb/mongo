// Test partial index creation and drops.
//
// @tags: [
//     # Builds index in the background
//     requires_background_index,
//     uses_full_validation,
//     requires_getmore,
// ]

import {IndexUtils} from "jstests/libs/index_utils.js";

let coll = db.index_partial_create_drop;

let getNumKeys = function (idxName) {
    const res = assert.commandWorked(coll.validate({full: true}));

    // If collection is sharded, accumulate number of keys from all shards.
    const isShardedColl = res.hasOwnProperty("raw");
    if (isShardedColl) {
        let totalKpi = 0;
        for (let shardId in res.raw) {
            const kpi = res.raw[shardId].keysPerIndex[idxName];
            if (kpi) {
                totalKpi += kpi;
            }
        }
        return totalKpi;
    }

    return res.keysPerIndex[idxName];
};

coll.drop();

// Check bad filter spec on create.
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: 5}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {x: {$asdasd: 3}}}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {$and: 5}}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {x: /abc/}}));

// Use of $expr is banned in a partial index filter.
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {$expr: {$eq: ["$x", 5]}}}));
assert.commandFailed(
    coll.createIndex({x: 1}, {partialFilterExpression: {$expr: {$eq: [{$trim: {input: "$x"}}, "hi"]}}}),
);

// Tree depth cannot exceed `internalPartialFilterExpressionMaxDepth`, which defaults to 4.
assert.commandFailedWithCode(
    coll.createIndex({x: 1}, {partialFilterExpression: {$and: [{$and: [{$and: [{$and: [{x: 3}]}]}]}]}}),
    ErrorCodes.CannotCreateIndex,
);

// A tree depth of `internalPartialFilterExpressionMaxDepth` is allowed.
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {$and: [{$and: [{$and: [{x: 3}]}]}]}}));
assert(coll.drop());

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({x: i, a: i}));
}

// Create partial index.
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}}}));
assert.eq(5, getNumKeys("x_1"));
assert(IndexUtils.indexExists(coll, {x: 1}, {partialFilterExpression: {a: {$lt: 5}}}), coll.getIndexes());

assert.commandWorked(coll.dropIndex({x: 1}));
assert(!IndexUtils.indexExists(coll, {x: 1}, {partialFilterExpression: {a: {$lt: 5}}}), coll.getIndexes());
IndexUtils.assertIndexes(coll, [{_id: 1}]);

// Create partial index in background.
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}}}));
assert.eq(5, getNumKeys("x_1"));
assert(IndexUtils.indexExists(coll, {x: 1}, {partialFilterExpression: {a: {$lt: 5}}}), coll.getIndexes());

assert.commandWorked(coll.dropIndex({x: 1}));
assert(!IndexUtils.indexExists(coll, {x: 1}, {partialFilterExpression: {a: {$lt: 5}}}), coll.getIndexes());
IndexUtils.assertIndexes(coll, [{_id: 1}]);

// Create complete index, same key as previous indexes.
assert.commandWorked(coll.createIndex({x: 1}));
assert.eq(10, getNumKeys("x_1"));
IndexUtils.assertIndexes(coll, [{_id: 1}, {x: 1}]);

assert.commandWorked(coll.dropIndex({x: 1}));
IndexUtils.assertIndexes(coll, [{_id: 1}]);

// Partial indexes can't also be sparse indexes.
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: true}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: 1}));
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: false}));
assert(IndexUtils.indexExists(coll, {x: 1}, {partialFilterExpression: {a: 1}, sparse: false}), coll.getIndexes());

assert.commandWorked(coll.dropIndex({x: 1}));
IndexUtils.assertIndexes(coll, [{_id: 1}]);

// SERVER-18858: Verify that query compatible w/ partial index succeeds after index drop.
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}}}));
assert.commandWorked(coll.dropIndex({x: 1}));
IndexUtils.assertIndexes(coll, [{_id: 1}]);

// Can create multiple partial indexes on the same key pattern as long as the filter is different.
assert.commandWorked(coll.dropIndexes());
IndexUtils.assertIndexes(coll, [{_id: 1}]);

assert.commandWorked(coll.createIndex({x: 1}, {name: "partialIndex1", partialFilterExpression: {a: {$lt: 5}}}));
assert(
    IndexUtils.indexExists(coll, {x: 1}, {name: "partialIndex1", partialFilterExpression: {a: {$lt: 5}}}),
    coll.getIndexes(),
);
IndexUtils.assertIndexes(coll, [{_id: 1}, {x: 1}]);

assert.commandWorked(coll.createIndex({x: 1}, {name: "partialIndex2", partialFilterExpression: {a: {$gte: 5}}}));
assert(
    IndexUtils.indexExists(coll, {x: 1}, {name: "partialIndex2", partialFilterExpression: {a: {$gte: 5}}}),
    coll.getIndexes(),
);
IndexUtils.assertIndexes(coll, [{_id: 1}, {x: 1}, {x: 1}]);

// Index drop by key pattern fails when more than one index exists with the given key.
assert.commandFailedWithCode(coll.dropIndex({x: 1}), ErrorCodes.AmbiguousIndexKeyPattern);
IndexUtils.assertIndexes(coll, [{_id: 1}, {x: 1}, {x: 1}]);

assert.commandWorked(coll.dropIndex("partialIndex2"));
assert(
    !IndexUtils.indexExists(coll, {x: 1}, {name: "partialIndex2", partialFilterExpression: {a: {$gte: 5}}}),
    coll.getIndexes(),
);
assert(
    IndexUtils.indexExists(coll, {x: 1}, {name: "partialIndex1", partialFilterExpression: {a: {$lt: 5}}}),
    coll.getIndexes(),
);
