// SERVER-9547
// Test that sorting with .max() and .min() doesn't crash.
// @tags: [
//   requires_getmore,
// ]

let t = db[jsTestName()];
t.drop();

for (let i = 0; i < 10; i++) {
    t.save({a: i});
}

t.createIndex({a: 1});

// note: max() value is exclusive upper bound
assert.eq(4, t.find({}).max({a: 4}).hint({a: 1}).toArray().length, "no order");

// Ascending order is fine.
assert.eq(4, t.find({}).max({a: 4}).sort({a: 1}).hint({a: 1}).toArray().length, "ascending");

// Descending order is fine.
assert.eq(4, t.find({}).max({a: 4}).sort({a: -1}).hint({a: 1}).toArray().length, "descending");
