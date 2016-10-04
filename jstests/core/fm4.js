t = db.fm4;
t.drop();

t.insert({_id: 1, a: 1, b: 1});

assert.eq(t.findOne({}, {_id: 1}), {_id: 1}, 1);
assert.eq(t.findOne({}, {_id: 0}), {a: 1, b: 1}, 2);

assert.eq(t.findOne({}, {_id: 1, a: 1}), {_id: 1, a: 1}, 3);
assert.eq(t.findOne({}, {_id: 0, a: 1}), {a: 1}, 4);

assert.eq(t.findOne({}, {_id: 0, a: 0}), {b: 1}, 6);
assert.eq(t.findOne({}, {a: 0}), {_id: 1, b: 1}, 5);

// not sure if we want to suport this since it is the same as above
// assert.eq( t.findOne({}, {_id:1, a:0}), {_id:1, b:1}, 5)
