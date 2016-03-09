
t = db.arrayfind3;
t.drop();

t.save({a: [1, 2]});
t.save({a: [1, 2, 6]});
t.save({a: [1, 4, 6]});

assert.eq(2, t.find({a: {$gte: 3, $lte: 5}}).itcount(), "A1");
assert.eq(1, t.find({a: {$elemMatch: {$gte: 3, $lte: 5}}}).itcount(), "A2");

t.ensureIndex({a: 1});

assert.eq(2, t.find({a: {$gte: 3, $lte: 5}}).itcount(), "B1");
assert.eq(1, t.find({a: {$elemMatch: {$gte: 3, $lte: 5}}}).itcount(), "B2");
