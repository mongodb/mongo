// @tags: [requires_non_retryable_writes]

let t = db.jstests_or7;
t.drop();

t.createIndex({a: 1});
t.save({a: 2});

assert.eq(1, t.count({$or: [{a: {$in: [1, 3]}}, {a: 2}]}));

// SERVER-1201 ...

t.remove({});

t.save({a: "aa"});
t.save({a: "ab"});
t.save({a: "ad"});

assert.eq(3, t.count({$or: [{a: /^ab/}, {a: /^a/}]}));

t.remove({});

t.save({a: "aa"});
t.save({a: "ad"});

assert.eq(2, t.count({$or: [{a: /^ab/}, {a: /^a/}]}));

t.remove({});

t.save({a: "aa"});
t.save({a: "ac"});

assert.eq(2, t.count({$or: [{a: /^ab/}, {a: /^a/}]}));

assert.eq(2, t.count({$or: [{a: /^ab/}, {a: /^a/}]}));

t.save({a: "ab"});
assert.eq(3, t.count({$or: [{a: {$in: [/^ab/], $gte: "abc"}}, {a: /^a/}]}));

t.remove({});
t.save({a: "a"});
t.save({a: "b"});
assert.eq(2, t.count({$or: [{a: {$gt: "a", $lt: "b"}}, {a: {$gte: "a", $lte: "b"}}]}));
