
t = db.in1;
t.drop();

t.save({a: 1});
t.save({a: 2});

// $in must take an array as argument: SERVER-7445
assert.throws(function() {
    return t.find({a: {$in: {x: 1}}}).itcount();
});
assert.throws(function() {
    return t.find({a: {$in: 1}}).itcount();
});

assert.eq(1, t.find({a: {$in: [1]}}).itcount(), "A");
assert.eq(1, t.find({a: {$in: [2]}}).itcount(), "B");
assert.eq(2, t.find({a: {$in: [1, 2]}}).itcount(), "C");

t.ensureIndex({a: 1});

assert.eq(1, t.find({a: {$in: [1]}}).itcount(), "D");
assert.eq(1, t.find({a: {$in: [2]}}).itcount(), "E");
assert.eq(2, t.find({a: {$in: [1, 2]}}).itcount(), "F");

assert.eq(0, t.find({a: {$in: []}}).itcount(), "G");

assert.eq(1, t.find({a: {$gt: 1, $in: [2]}}).itcount(), "H");
