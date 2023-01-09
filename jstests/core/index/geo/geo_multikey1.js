// Multikey geo index tests with parallel arrays.

t = db.jstests_geo_multikey1;
t.drop();

locArr = [];
arr = [];
for (i = 0; i < 10; ++i) {
    locArr.push([i, i + 1]);
    arr.push(i);
}
t.save({loc: locArr, a: arr, b: arr, c: arr});

// Parallel arrays are allowed for geo indexes.
assert.commandWorked(t.createIndex({loc: '2d', a: 1, b: 1, c: 1}));

// Parallel arrays are not allowed for normal indexes.
assert.commandFailed(t.createIndex({loc: 1, a: 1, b: 1, c: 1}));
