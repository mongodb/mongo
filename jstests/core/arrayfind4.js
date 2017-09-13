// Test query empty array SERVER-2258

t = db.jstests_arrayfind4;
t.drop();

t.save({a: []});
t.ensureIndex({a: 1});

assert.eq(1, t.find({a: []}).hint({$natural: 1}).itcount());
assert.eq(1, t.find({a: []}).hint({a: 1}).itcount());

assert.eq(1, t.find({a: {$in: [[]]}}).hint({$natural: 1}).itcount());
assert.eq(1, t.find({a: {$in: [[]]}}).hint({a: 1}).itcount());

t.remove({});
t.save({a: [[]]});

assert.eq(1, t.find({a: []}).hint({$natural: 1}).itcount());
assert.eq(1, t.find({a: []}).hint({a: 1}).itcount());

assert.eq(1, t.find({a: {$in: [[]]}}).hint({$natural: 1}).itcount());
assert.eq(1, t.find({a: {$in: [[]]}}).hint({a: 1}).itcount());
