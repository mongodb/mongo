/**
 * SERVER-85577: We do optimize for unique point bound index scan to avoid unnecessary
 * cursor->next() call, but missing fields for compound index may lead incorrect point bound
 * validation even though keyStrings are equal (without discriminator).
 *
 * Cannot implicitly shard accessed collections because of not being able to create unique index
 * using hashed shard key pattern.
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */

let coll = db.agg_sample;
coll.drop();

assert.commandWorked(coll.insert({a: 0, b: 1}));
assert.commandWorked(coll.insert({a: 0, b: 2}));

assert.commandWorked(coll.createIndex({"a": 1, "b": 1}, {unique: true}));

// $lookup joining with field 'a' will make a idx scan stage on keyString with field 'a' only, but
// the index is a_1_b_1, {a: 0} shouldn't be treated as a point bound.
let resultsAscending = coll
    .aggregate([{$limit: 1}, {$lookup: {from: coll.getName(), localField: "a", foreignField: "a", as: "array"}}])
    .toArray();
assert.eq(resultsAscending.length, 1, resultsAscending);
assert.eq(resultsAscending[0].array.length, 2, resultsAscending);

assert.commandWorked(coll.dropIndex({"a": 1, "b": 1}));

assert.commandWorked(coll.createIndex({"a": 1, "b": -1}, {unique: true}));

let resultsDescending = coll
    .aggregate([{$limit: 1}, {$lookup: {from: coll.getName(), localField: "a", foreignField: "a", as: "array"}}])
    .toArray();
assert.eq(resultsDescending.length, 1, resultsDescending);
assert.eq(resultsDescending[0].array.length, 2, resultsDescending);
