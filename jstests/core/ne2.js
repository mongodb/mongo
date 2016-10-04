// check that we don't scan $ne values

t = db.jstests_ne2;
t.drop();
t.ensureIndex({a: 1});

t.save({a: -0.5});
t.save({a: 0});
t.save({a: 0});
t.save({a: 0.5});

e = t.find({a: {$ne: 0}}).explain(true);
assert.eq(2, e.executionStats.nReturned, 'A');

e = t.find({a: {$gt: -1, $lt: 1, $ne: 0}}).explain(true);
assert.eq(2, e.executionStats.nReturned, 'B');
