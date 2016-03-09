
t = db.ne1;
t.drop();

t.save({x: 1});
t.save({x: 2});
t.save({x: 3});

assert.eq(2, t.find({x: {$ne: 2}}).itcount(), "A");
t.ensureIndex({x: 1});
assert.eq(2, t.find({x: {$ne: 2}}).itcount(), "B");
