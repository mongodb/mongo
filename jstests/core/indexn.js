// Test "impossible match" queries, or queries that will always have
// an empty result set.

t = db.jstests_indexn;
t.drop();

t.save({a: 1, b: [1, 2]});

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

// {a:1} is a single key index, so no matches are possible for this query
assert.eq(0, t.count({a: {$gt: 5, $lt: 0}}));

assert.eq(0, t.count({a: {$gt: 5, $lt: 0}, b: 2}));

assert.eq(0, t.count({a: {$gt: 5, $lt: 0}, b: {$gt: 0, $lt: 5}}));

// One clause of an $or is an "impossible match"
printjson(t.find({$or: [{a: {$gt: 5, $lt: 0}}, {a: 1}]}).explain());
assert.eq(1, t.count({$or: [{a: {$gt: 5, $lt: 0}}, {a: 1}]}));

// One clause of an $or is an "impossible match"; original order of the $or
// does not matter.
printjson(t.find({$or: [{a: 1}, {a: {$gt: 5, $lt: 0}}]}).explain());
assert.eq(1, t.count({$or: [{a: 1}, {a: {$gt: 5, $lt: 0}}]}));

t.save({a: 2});
assert.eq(2, t.count({$or: [{a: 1}, {a: {$gt: 5, $lt: 0}}, {a: 2}]}));
