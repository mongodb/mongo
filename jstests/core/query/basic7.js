
t = db.basic7;
t.drop();

t.save({a: 1});
t.createIndex({a: 1});

assert.eq(t.find().toArray()[0].a, 1);
assert.eq(t.find().arrayAccess(0).a, 1);
assert.eq(t.find()[0].a, 1);
