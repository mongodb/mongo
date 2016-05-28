
t = db.distinct_index1;
t.drop();

function r(x) {
    return Math.floor(Math.sqrt(x * 123123)) % 10;
}

function d(k, q) {
    return t.runCommand("distinct", {key: k, query: q || {}});
}

for (i = 0; i < 1000; i++) {
    o = {a: r(i * 5), b: r(i)};
    t.insert(o);
}

x = d("a");
// Collection scan looks at all 1000 documents and gets 1000
// distinct values. Looks at 0 index keys.
assert.eq(1000, x.stats.n, "AA1");
assert.eq(0, x.stats.nscanned, "AA2");
assert.eq(1000, x.stats.nscannedObjects, "AA3");

x = d("a", {a: {$gt: 5}});
// Collection scan looks at all 1000 documents and gets 398
// distinct values which match the query. Looks at 0 index keys.
assert.eq(398, x.stats.n, "AB1");
assert.eq(0, x.stats.nscanned, "AB2");
assert.eq(1000, x.stats.nscannedObjects, "AB3");

x = d("b", {a: {$gt: 5}});
// Collection scan looks at all 1000 documents and gets 398
// distinct values which match the query. Looks at 0 index keys.
assert.eq(398, x.stats.n, "AC1");
assert.eq(0, x.stats.nscanned, "AC2");
assert.eq(1000, x.stats.nscannedObjects, "AC3");

t.ensureIndex({a: 1});

x = d("a");
// There are only 10 values.  We use the fast distinct hack and only examine each value once.
assert.eq(10, x.stats.n, "BA1");
assert.eq(10, x.stats.nscanned, "BA2");

x = d("a", {a: {$gt: 5}});
// Only 4 values of a are >= 5 and we use the fast distinct hack.
assert.eq(4, x.stats.n, "BB1");
assert.eq(4, x.stats.nscanned, "BB2");
assert.eq(0, x.stats.nscannedObjects, "BB3");

x = d("b", {a: {$gt: 5}});
// We can't use the fast distinct hack here because we're distinct-ing over 'b'.
assert.eq(398, x.stats.n, "BC1");
assert.eq(398, x.stats.nscanned, "BC2");
assert.eq(398, x.stats.nscannedObjects, "BC3");

// Check proper nscannedObjects count when using a query optimizer cursor.
t.dropIndexes();
t.ensureIndex({a: 1, b: 1});
x = d("b", {a: {$gt: 5}, b: {$gt: 5}});
printjson(x);
// 171 is the # of results we happen to scan when we don't use a distinct
// hack.  When we use the distinct hack we scan 16, currently.
assert.lte(x.stats.n, 171);
assert.eq(171, x.stats.nscannedObjects, "BD3");

// Should use an index scan over the hashed index.
t.dropIndexes();
t.ensureIndex({a: "hashed"});
x = d("a", {$or: [{a: 3}, {a: 5}]});
assert.eq(188, x.stats.n, "DA1");
assert.eq("IXSCAN { a: \"hashed\" }", x.stats.planSummary);
