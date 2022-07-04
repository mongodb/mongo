// @tags: [
//     # Cannot implicitly shard accessed collections because of extra shard key index in sharded
//     # collection.
//     assumes_no_implicit_index_creation,
//
//     # Builds index in the background
//     requires_background_index,
//
//     uses_full_validation,
//     requires_fcv_51,
//
// ]

// Test partial index creation and drops.

load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";
var coll = db.index_partial_create_drop;

var getNumKeys = function(idxName) {
    var res = assert.commandWorked(coll.validate({full: true}));
    var kpi;

    var isShardedNS = res.hasOwnProperty('raw');
    if (isShardedNS) {
        kpi = res.raw[Object.getOwnPropertyNames(res.raw)[0]].keysPerIndex;
    } else {
        kpi = res.keysPerIndex;
    }
    return kpi[idxName];
};

coll.drop();

// Check bad filter spec on create.
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: 5}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {x: {$asdasd: 3}}}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {$and: 5}}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {x: /abc/}}));

// Use of $expr is banned in a partial index filter.
assert.commandFailed(
    coll.createIndex({x: 1}, {partialFilterExpression: {$expr: {$eq: ["$x", 5]}}}));
assert.commandFailed(coll.createIndex(
    {x: 1}, {partialFilterExpression: {$expr: {$eq: [{$trim: {input: "$x"}}, "hi"]}}}));

if (!FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
    // Only top-level $and is permitted in a partial filter expression.
    assert.commandFailedWithCode(coll.createIndex({x: 1}, {
        partialFilterExpression:
            {$and: [{$and: [{x: {$lt: 2}}, {x: {$gt: 0}}]}, {x: {$exists: true}}]}
    }),
                                 ErrorCodes.CannotCreateIndex);
} else {
    // Tree depth cannot exceed `internalPartialFilterExpressionMaxDepth`, which defaults to 4.
    assert.commandFailedWithCode(
        coll.createIndex({x: 1},
                         {partialFilterExpression: {$and: [{$and: [{$and: [{$and: [{x: 3}]}]}]}]}}),
        ErrorCodes.CannotCreateIndex);

    // A tree depth of `internalPartialFilterExpressionMaxDepth` is allowed.
    assert.commandWorked(
        coll.createIndex({x: 1}, {partialFilterExpression: {$and: [{$and: [{$and: [{x: 3}]}]}]}}));
    assert(coll.drop());
}

for (var i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({x: i, a: i}));
}

// Create partial index.
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}}}));
assert.eq(5, getNumKeys("x_1"));
assert.commandWorked(coll.dropIndex({x: 1}));
assert.eq(1, coll.getIndexes().length);

// Create partial index in background.
assert.commandWorked(
    coll.createIndex({x: 1}, {background: true, partialFilterExpression: {a: {$lt: 5}}}));
assert.eq(5, getNumKeys("x_1"));
assert.commandWorked(coll.dropIndex({x: 1}));
assert.eq(1, coll.getIndexes().length);

// Create complete index, same key as previous indexes.
assert.commandWorked(coll.createIndex({x: 1}));
assert.eq(10, getNumKeys("x_1"));
assert.commandWorked(coll.dropIndex({x: 1}));
assert.eq(1, coll.getIndexes().length);

// Partial indexes can't also be sparse indexes.
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: true}));
assert.commandFailed(coll.createIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: 1}));
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: 1}, sparse: false}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.dropIndex({x: 1}));
assert.eq(1, coll.getIndexes().length);

// SERVER-18858: Verify that query compatible w/ partial index succeeds after index drop.
assert.commandWorked(coll.createIndex({x: 1}, {partialFilterExpression: {a: {$lt: 5}}}));
assert.commandWorked(coll.dropIndex({x: 1}));
assert.eq(1, coll.find({x: 0, a: 0}).itcount());

// Can create multiple partial indexes on the same key pattern as long as the filter is different.
assert.commandWorked(coll.dropIndexes());

let numIndexesBefore = coll.getIndexes().length;
assert.commandWorked(
    coll.createIndex({x: 1}, {name: "partialIndex1", partialFilterExpression: {a: {$lt: 5}}}));
assert.eq(coll.getIndexes().length, numIndexesBefore + 1);

numIndexesBefore = coll.getIndexes().length;
assert.commandWorked(
    coll.createIndex({x: 1}, {name: "partialIndex2", partialFilterExpression: {a: {$gte: 5}}}));
assert.eq(coll.getIndexes().length, numIndexesBefore + 1);

// Index drop by key pattern fails when more than one index exists with the given key.
numIndexesBefore = coll.getIndexes().length;
assert.commandFailedWithCode(coll.dropIndex({x: 1}), ErrorCodes.AmbiguousIndexKeyPattern);
assert.commandWorked(coll.dropIndex("partialIndex2"));
assert.eq(coll.getIndexes().length, numIndexesBefore - 1);
})();
