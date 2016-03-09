
t = db.indexf;
t.drop();

t.ensureIndex({x: 1});

t.save({x: 2});
t.save({y: 3});
t.save({x: 4});

assert.eq(2, t.findOne({x: 2}).x, "A1");
assert.eq(3, t.findOne({x: null}).y, "A2");
assert.eq(4, t.findOne({x: 4}).x, "A3");
