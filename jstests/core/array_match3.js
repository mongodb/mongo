// SERVER-2902 Test indexing of numerically referenced array elements.

t = db.jstests_array_match3;
t.drop();

// Test matching numericallly referenced array element.
t.save({a: {'0': 5}});
t.save({a: [5]});
assert.eq(2, t.count({'a.0': 5}));

// Test with index.
t.ensureIndex({'a.0': 1});
assert.eq(2, t.count({'a.0': 5}));
