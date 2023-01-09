t = db.getCollection("basic5");
t.drop();

t.save({a: 1, b: [1, 2, 3]});
assert.eq(3, t.findOne().b.length);
