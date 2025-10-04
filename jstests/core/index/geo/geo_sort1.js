// @tags: [
//   requires_getmore,
// ]

let t = db.geo_sort1;
t.drop();

for (let x = 0; x < 10; x++) {
    for (let y = 0; y < 10; y++) {
        t.insert({loc: [x, y], foo: x * x * y});
    }
}

t.createIndex({loc: "2d", foo: 1});

let q = t.find({loc: {$near: [5, 5]}, foo: {$gt: 20}});
let m = function (z) {
    return z.foo;
};

let a = q.clone().map(m);
let b = q.clone().sort({foo: 1}).map(m);

assert.neq(a, b, "A");
a.sort();
b.sort();
assert.eq(a, b, "B");
