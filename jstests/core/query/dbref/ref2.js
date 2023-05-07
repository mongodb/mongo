// @tags: [requires_fastcount]

let t = db.ref2;
t.drop();

let a = {$ref: "foo", $id: 1};
let b = {$ref: "foo", $id: 2};

t.save({name: "a", r: a});
t.save({name: "b", r: b});

assert.eq(2, t.find().count(), "A");
assert.eq(1, t.find({r: a}).count(), "B");
assert.eq(1, t.find({r: b}).count(), "C");
