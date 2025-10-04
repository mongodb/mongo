// Write ops tests for partial indexes.
// @tags: [
//    cannot_create_unique_index_when_using_hashed_shard_key,
//    requires_non_retryable_writes,
//    uses_full_validation,
// ]

let coll = db.index_partial_write_ops;

let getNumKeys = function (idxName) {
    let res = assert.commandWorked(coll.validate({full: true}));
    let kpi;

    let isShardedNS = res.hasOwnProperty("raw");
    if (isShardedNS) {
        kpi = res.raw[Object.getOwnPropertyNames(res.raw)[0]].keysPerIndex;
    } else {
        kpi = res.keysPerIndex;
    }
    return kpi[idxName];
};

coll.drop();

// Create partial index.
assert.commandWorked(coll.createIndex({x: 1}, {unique: true, partialFilterExpression: {a: 1}}));

assert.commandWorked(coll.insert({_id: 1, x: 5, a: 2, b: 1})); // Not in index.
assert.commandWorked(coll.insert({_id: 2, x: 6, a: 1, b: 1})); // In index.

assert.eq(1, getNumKeys("x_1"));

// Move into partial index, then back out.
assert.commandWorked(coll.update({_id: 1}, {$set: {a: 1}}));
assert.eq(2, getNumKeys("x_1"));

assert.commandWorked(coll.update({_id: 1}, {$set: {a: 2}}));
assert.eq(1, getNumKeys("x_1"));

// Bit blip doc in partial index, and out of partial index.
assert.commandWorked(coll.update({_id: 2}, {$set: {b: 2}}));
assert.eq(1, getNumKeys("x_1"));

assert.commandWorked(coll.update({_id: 1}, {$set: {b: 2}}));
assert.eq(1, getNumKeys("x_1"));

let array = [];
for (let i = 0; i < 2048; i++) {
    array.push({arbitrary: i});
}

// Update that causes record relocation.
assert.commandWorked(coll.update({_id: 2}, {$set: {b: array}}));
assert.eq(1, getNumKeys("x_1"));

assert.commandWorked(coll.update({_id: 1}, {$set: {b: array}}));
assert.eq(1, getNumKeys("x_1"));

// Delete that doesn't affect partial index.
assert.commandWorked(coll.remove({x: 5}));
assert.eq(1, getNumKeys("x_1"));

// Delete that does affect partial index.
assert.commandWorked(coll.remove({x: 6}));
assert.eq(0, getNumKeys("x_1"));

// Documents with duplicate keys that straddle the index.
assert.commandWorked(coll.insert({_id: 3, x: 1, a: 1})); // In index.
assert.commandWorked(coll.insert({_id: 4, x: 1, a: 0})); // Not in index.
assert.writeErrorWithCode(coll.insert({_id: 5, x: 1, a: 1}), ErrorCodes.DuplicateKey); // Duplicate key constraint prevents insertion.

// Only _id 3 is in the index.
assert.eq(1, getNumKeys("x_1"));

// Remove _id 4, _id 3 should remain in index.
assert.commandWorked(coll.remove({_id: 4}));

// _id 3 is still in the index.
assert.eq(1, getNumKeys("x_1"));
