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

var coll = db.agg_sample;
coll.drop();

assert.commandWorked(coll.insert({a: 0, b: 1}));
assert.commandWorked(coll.insert({a: 0, b: 2}));

assert.commandWorked(coll.createIndex({'a': 1, 'b': 1}, {unique: true}));

// $lookup joining with field 'a' will make a idx scan stage on keyString with field 'a' only, but
// the index is a_1_b_1, {a: 0} shouldn't be treated as a point bound.
var results =
    coll.aggregate([
            {$limit: 1},
            {$lookup: {from: coll.getName(), localField: 'a', foreignField: 'a', as: 'array'}}
        ])
        .toArray();
assert.eq(results.length, 1, results);
assert.eq(results[0].array.length, 2, results);
