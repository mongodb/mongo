// SERVER-393 Test exists with various empty array and empty object cases.

t = db.jstests_exists9;
t.drop();

// Check existence of missing nested field.
t.save({a: {}});
assert.eq(1, t.count({'a.b': {$exists: false}}));
assert.eq(0, t.count({'a.b': {$exists: true}}));

// With index.
t.ensureIndex({'a.b': 1});
assert.eq(1, t.find({'a.b': {$exists: false}}).hint({'a.b': 1}).itcount());
assert.eq(0, t.find({'a.b': {$exists: true}}).hint({'a.b': 1}).itcount());

t.drop();

// Check that an empty array 'exists'.
t.save({});
t.save({a: []});
assert.eq(1, t.count({a: {$exists: true}}));
assert.eq(1, t.count({a: {$exists: false}}));

// With index.
t.ensureIndex({a: 1});
assert.eq(1, t.find({a: {$exists: true}}).hint({a: 1}).itcount());
assert.eq(1, t.find({a: {$exists: false}}).hint({a: 1}).itcount());

t.drop();

// Check that an indexed field within an empty array does not exist.
t.save({a: {'0': 1}});
t.save({a: []});
assert.eq(1, t.count({'a.0': {$exists: true}}));
assert.eq(1, t.count({'a.0': {$exists: false}}));

// With index.
t.ensureIndex({'a.0': 1});
assert.eq(1, t.find({'a.0': {$exists: true}}).hint({'a.0': 1}).itcount());
assert.eq(1, t.find({'a.0': {$exists: false}}).hint({'a.0': 1}).itcount());
