
t = db.drop_undefined.js;

t.insert({_id: 1});
t.insert({_id: 2});
t.insert({_id: null});

z = {
    foo: 1,
    x: null
};

t.remove({x: z.bar});
assert.eq(3, t.count(), "A1");

t.remove({x: undefined});
assert.eq(3, t.count(), "A2");

assert.throws(function() {
    t.remove({_id: z.bar});
}, [], "B1");
assert.throws(function() {
    t.remove({_id: undefined});
}, [], "B2");

t.remove({_id: z.x});
assert.eq(2, t.count(), "C1");

t.insert({_id: null});
assert.eq(3, t.count(), "C2");

assert.throws(function() {
    t.remove({_id: undefined});
}, [], "C3");
assert.eq(3, t.count(), "C4");
