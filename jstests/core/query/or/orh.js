// @tags: [requires_non_retryable_writes]

// SERVER-2831 Demonstration of sparse index matching semantics in a multi index $or query.

let t = db.jstests_orh;
t.drop();

t.createIndex({a: 1}, {sparse: true});
t.createIndex({b: 1, a: 1});

t.remove({});
t.save({b: 2});
assert.eq(1, t.count({a: null}));
assert.eq(1, t.count({b: 2, a: null}));

assert.eq(1, t.count({$or: [{b: 2, a: null}, {a: null}]}));

// Is this desired?
assert.eq(1, t.count({$or: [{a: null}, {b: 2, a: null}]}));
