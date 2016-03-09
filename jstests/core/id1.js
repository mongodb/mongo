
t = db.id1;
t.drop();

t.save({_id: {a: 1, b: 2}, x: "a"});
t.save({_id: {a: 1, b: 2}, x: "b"});
t.save({_id: {a: 3, b: 2}, x: "c"});
t.save({_id: {a: 4, b: 2}, x: "d"});
t.save({_id: {a: 4, b: 2}, x: "e"});
t.save({_id: {a: 2, b: 2}, x: "f"});

assert.eq(4, t.find().count(), "A");
assert.eq("b", t.findOne({_id: {a: 1, b: 2}}).x);
assert.eq("c", t.findOne({_id: {a: 3, b: 2}}).x);
assert.eq("e", t.findOne({_id: {a: 4, b: 2}}).x);
assert.eq("f", t.findOne({_id: {a: 2, b: 2}}).x);
