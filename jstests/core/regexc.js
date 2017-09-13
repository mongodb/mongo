// Multiple regular expressions using the same index

var t = db.jstests_regexc;

// $and using same index twice
t.drop();
t.ensureIndex({a: 1});
t.save({a: "0"});
t.save({a: "1"});
t.save({a: "10"});
assert.eq(1, t.find({$and: [{a: /0/}, {a: /1/}]}).itcount());

// implicit $and using compound index twice
t.drop();
t.ensureIndex({a: 1, b: 1});
t.save({a: "0", b: "1"});
t.save({a: "10", b: "10"});
t.save({a: "10", b: "2"});
assert.eq(2, t.find({a: /0/, b: /1/}).itcount());

// $or using same index twice
t.drop();
t.ensureIndex({a: 1});
t.save({a: "0"});
t.save({a: "1"});
t.save({a: "2"});
t.save({a: "10"});
assert.eq(3, t.find({$or: [{a: /0/}, {a: /1/}]}).itcount());
