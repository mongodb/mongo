// Test a query with 200 $or clauses

t = db.jstests_orf;
t.drop();

var a = [];
var expectBounds = [];
for (var i = 0; i < 200; ++i) {
    a.push({_id: i});
    expectBounds.push([i, i]);
}
a.forEach(function(x) {
    t.save(x);
});

// This $or query is answered as an index scan over
// a series of _id index point intervals.
explain = t.find({$or: a}).hint({_id: 1}).explain(true);
printjson(explain);
assert.eq(200, explain.executionStats.nReturned, 'n');
assert.eq(200, explain.executionStats.totalKeysExamined, 'keys examined');
assert.eq(200, explain.executionStats.totalDocsExamined, 'docs examined');

assert.eq(200, t.count({$or: a}));
