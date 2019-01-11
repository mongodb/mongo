// @tags: [assumes_balancer_off]
t = db.distinct_index1;
t.drop();

load("jstests/libs/analyze_plan.js");

function r(x) {
    return Math.floor(Math.sqrt(x * 123123)) % 10;
}

function d(k, q) {
    return t.explain("executionStats").distinct(k, q || {});
}

for (i = 0; i < 1000; i++) {
    o = {a: r(i * 5), b: r(i)};
    t.insert(o);
}

x = d("a");
// Collection scan looks at all 1000 documents and gets 1000
// distinct values. Looks at 0 index keys.
assert.eq(1000, x.executionStats.nReturned);
assert.eq(0, x.executionStats.totalKeysExamined);
assert.eq(1000, x.executionStats.totalDocsExamined);

x = d("a", {a: {$gt: 5}});
// Collection scan looks at all 1000 documents and gets 398
// distinct values which match the query. Looks at 0 index keys.
assert.eq(398, x.executionStats.nReturned);
assert.eq(0, x.executionStats.totalKeysExamined);
assert.eq(1000, x.executionStats.totalDocsExamined);

x = d("b", {a: {$gt: 5}});
// Collection scan looks at all 1000 documents and gets 398
// distinct values which match the query. Looks at 0 index keys.
assert.eq(398, x.executionStats.nReturned);
assert.eq(0, x.executionStats.totalKeysExamined);
assert.eq(1000, x.executionStats.totalDocsExamined);

t.ensureIndex({a: 1});

x = d("a");
// There are only 10 values.  We use the fast distinct hack and only examine each value once.
assert.eq(10, x.executionStats.nReturned);
assert.lte(10, x.executionStats.totalKeysExamined);

x = d("a", {a: {$gt: 5}});
// Only 4 values of a are >= 5 and we use the fast distinct hack.
assert.eq(4, x.executionStats.nReturned);
assert.eq(4, x.executionStats.totalKeysExamined);
assert.eq(0, x.executionStats.totalDocsExamined);

x = d("b", {a: {$gt: 5}});
// We can't use the fast distinct hack here because we're distinct-ing over 'b'.
assert.eq(398, x.executionStats.nReturned);
assert.eq(398, x.executionStats.totalKeysExamined);
assert.eq(398, x.executionStats.totalDocsExamined);

// Test that a distinct over a trailing field of the index can be covered.
t.dropIndexes();
t.ensureIndex({a: 1, b: 1});
x = d("b", {a: {$gt: 5}, b: {$gt: 5}});
printjson(x);
assert.lte(x.executionStats.nReturned, 171);
assert.eq(0, x.executionStats.totalDocsExamined);

// Should use an index scan over the hashed index.
t.dropIndexes();
t.ensureIndex({a: "hashed"});
x = d("a", {$or: [{a: 3}, {a: 5}]});
assert.eq(188, x.executionStats.nReturned);
var indexScanStage = getPlanStage(x.executionStats.executionStages, "IXSCAN");
assert.eq("hashed", indexScanStage.keyPattern.a);
