
t = db.basic6;

t.findOne();
t.a.findOne();

assert.eq("test.basic6", t.toString());
assert.eq("test.basic6.a", t.a.toString());
