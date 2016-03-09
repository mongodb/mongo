
t = db.geo_sort1;
t.drop();

for (x = 0; x < 10; x++) {
    for (y = 0; y < 10; y++) {
        t.insert({loc: [x, y], foo: x * x * y});
    }
}

t.ensureIndex({loc: "2d", foo: 1});

q = t.find({loc: {$near: [5, 5]}, foo: {$gt: 20}});
m = function(z) {
    return z.foo;
};

a = q.clone().map(m);
b = q.clone().sort({foo: 1}).map(m);

assert.neq(a, b, "A");
a.sort();
b.sort();
assert.eq(a, b, "B");
