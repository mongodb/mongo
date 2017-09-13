// SERVER-393 Test indexed matching with $exists.

t = db.jstests_exists6;
t.drop();

t.ensureIndex({b: 1});
t.save({});
t.save({b: 1});
t.save({b: null});

assert.eq(2, t.find({b: {$exists: true}}).itcount());
assert.eq(2, t.find({b: {$not: {$exists: false}}}).itcount());
assert.eq(1, t.find({b: {$exists: false}}).itcount());
assert.eq(1, t.find({b: {$not: {$exists: true}}}).itcount());

// Now check existence of second compound field.
t.ensureIndex({a: 1, b: 1});
t.save({a: 1});
t.save({a: 1, b: 1});
t.save({a: 1, b: null});

assert.eq(2, t.find({a: 1, b: {$exists: true}}).itcount());
assert.eq(2, t.find({a: 1, b: {$not: {$exists: false}}}).itcount());
assert.eq(1, t.find({a: 1, b: {$exists: false}}).itcount());
assert.eq(1, t.find({a: 1, b: {$not: {$exists: true}}}).itcount());
