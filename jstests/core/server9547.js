// SERVER-9547
// Test that sorting with .max() and .min() doesn't crash.

var t = db.server9547;
t.drop();

for (var i = 0; i < 10; i++) {
    t.save({a: i});
}

t.ensureIndex({a: 1});

// note: max() value is exclusive upper bound
assert.eq(4, t.find({}).max({a: 4}).toArray().length, "no order");

// Ascending order is fine.
assert.eq(4, t.find({}).max({a: 4}).sort({a: 1}).toArray().length, "ascending");

// Descending order is still broken.
// This should really return the same # of results but doesn't.
assert.eq(5, t.find({}).max({a: 4}).sort({a: -1}).toArray().length, "descending");
