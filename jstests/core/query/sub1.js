// sub1.js

let t = db.sub1;
t.drop();

let x = {a: 1, b: {c: {d: 2}}};

t.save(x);

let y = t.findOne();

assert.eq(1, y.a);
assert.eq(2, y.b.c.d);
print(tojson(y));
