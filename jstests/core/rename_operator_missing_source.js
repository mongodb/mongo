/*
 * Check some $rename cases with a missing source.  SERVER-4845
 *
 * @tags: [
 *   requires_non_retryable_writes,
 *   # update with multi:false is not supported on sharded collection
 *   assumes_unsharded_collection,
 * ]
 */

t = db.jstests_rename5;
t.drop();

t.createIndex({a: 1});
t.save({b: 1});

assert.writeOK(t.update({}, {$rename: {a: 'b'}}));
assert.eq(1, t.findOne().b);

// Test with another modifier.
assert.writeOK(t.update({}, {$rename: {a: 'b'}, $set: {x: 1}}));
assert.eq(1, t.findOne().b);
assert.eq(1, t.findOne().x);

// Test with an in place modifier.
assert.writeOK(t.update({}, {$rename: {a: 'b'}, $inc: {x: 1}}));
assert.eq(1, t.findOne().b);
assert.eq(2, t.findOne().x);

// Check similar cases with upserts.
t.drop();

t.remove({});
assert.writeOK(t.update({b: 1}, {$rename: {a: 'b'}}, true));
assert.eq(1, t.findOne().b);

t.remove({});
assert.writeOK(t.update({b: 1}, {$rename: {a: 'b'}, $set: {c: 1}}, true));
assert.eq(1, t.findOne().b);
assert.eq(1, t.findOne().c);

t.remove({});
assert.writeOK(t.update({b: 1, c: 2}, {$rename: {a: 'b'}, $inc: {c: 1}}, true));
assert.eq(1, t.findOne().b);
assert.eq(3, t.findOne().c);

// Check a similar case with multiple renames of an unindexed document.
t.drop();

t.save({b: 1, x: 1});
assert.writeOK(t.update({}, {$rename: {a: 'b', x: 'y'}}));
assert.eq(1, t.findOne().b);
assert.eq(1, t.findOne().y);
assert(!t.findOne().x);
