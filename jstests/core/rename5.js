// Check some $rename cases with a missing source.  SERVER-4845

t = db.jstests_rename5;
t.drop();

t.ensureIndex({a: 1});
t.save({b: 1});

t.update({}, {$rename: {a: 'b'}});
assert.eq(1, t.findOne().b);

// Test with another modifier.
t.update({}, {$rename: {a: 'b'}, $set: {x: 1}});
assert.eq(1, t.findOne().b);
assert.eq(1, t.findOne().x);

// Test with an in place modifier.
t.update({}, {$rename: {a: 'b'}, $inc: {x: 1}});
assert.eq(1, t.findOne().b);
assert.eq(2, t.findOne().x);

// Check similar cases with upserts.
t.drop();

t.remove({});
t.update({b: 1}, {$rename: {a: 'b'}}, true);
assert.eq(1, t.findOne().b);

t.remove({});
t.update({b: 1}, {$rename: {a: 'b'}, $set: {c: 1}}, true);
assert.eq(1, t.findOne().b);
assert.eq(1, t.findOne().c);

t.remove({});
t.update({b: 1, c: 2}, {$rename: {a: 'b'}, $inc: {c: 1}}, true);
assert.eq(1, t.findOne().b);
assert.eq(3, t.findOne().c);

// Check a similar case with multiple renames of an unindexed document.
t.drop();

t.save({b: 1, x: 1});
t.update({}, {$rename: {a: 'b', x: 'y'}});
assert.eq(1, t.findOne().b);
assert.eq(1, t.findOne().y);
assert(!t.findOne().x);
