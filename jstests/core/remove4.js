t = db.remove4;
t.drop();

t.save({a: 1, b: 1});
t.save({a: 2, b: 1});
t.save({a: 3, b: 1});

assert.eq(3, t.find().length());
t.remove({b: 1});
assert.eq(0, t.find().length());
