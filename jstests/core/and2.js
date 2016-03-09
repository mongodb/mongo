// Test dollar sign operator with $and SERVER-1089

t = db.jstests_and2;

t.drop();
t.save({a: [1, 2]});
t.update({a: 1}, {$set: {'a.$': 5}});
assert.eq([5, 2], t.findOne().a);

t.drop();
t.save({a: [1, 2]});
t.update({$and: [{a: 1}]}, {$set: {'a.$': 5}});
assert.eq([5, 2], t.findOne().a);
