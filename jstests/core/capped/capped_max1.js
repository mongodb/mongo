/**
 * Test max docs in capped collection
 *
 * @tags: [
 *   requires_fastcount,
 *   requires_collstats,
 *   requires_capped,
 *   # capped collections connot be sharded
 *   assumes_unsharded_collection,
 *   # SERVER-34918 The "max" option of a capped collection can be exceeded until the next insert.
 *   # The reason is that we don't update the count of a collection until a transaction commits,
 *   # by which point it is too late to complain that "max" has been exceeded.
 *   does_not_support_transactions,
 *   does_not_support_causal_consistency,
 *   # Does not support multiplanning, because it stashes results beyond batchSize.
 *   does_not_support_multiplanning_single_solutions,
 *   # This test relies on aggregations returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

let t = db[jsTestName()];
t.drop();

let max = 10;
let maxSize = 64 * 1024;
db.createCollection(t.getName(), {capped: true, size: maxSize, max: max});
assert.eq(max, t.stats().max);
assert.eq(maxSize, t.stats().maxSize);
assert.eq(Math.floor(maxSize / 1000), t.stats(1000).maxSize);

for (var i = 0; i < max * 2; i++) {
    t.insert({x: i});
}

assert.eq(max, t.count());

// Test invalidation of cursors
let cursor = t.find().batchSize(4);
assert(cursor.hasNext());
let myX = cursor.next();
for (let j = 0; j < max * 2; j++) {
    t.insert({x: j + i});
}

// Cursor should now be dead.
assert.throws(function () {
    cursor.toArray();
});
