// Check explain results for a plan that uses an index to obtain the requested sort order.

t = db.jstests_explain5;
t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

for (i = 0; i < 1000; ++i) {
    t.save({a: i, b: i % 3});
}

// Query with an initial set of documents.
var explain1 = t.find({a: {$gte: 0}, b: 2}).sort({a: 1}).hint({a: 1}).explain("executionStats");
printjson(explain1);
var stats1 = explain1.executionStats;
assert.eq(333, stats1.nReturned, 'wrong nReturned for explain1');
assert.eq(1000, stats1.totalKeysExamined, 'wrong totalKeysExamined for explain1');

for (i = 1000; i < 2000; ++i) {
    t.save({a: i, b: i % 3});
}

// Query with some additional documents.
var explain2 = t.find({a: {$gte: 0}, b: 2}).sort({a: 1}).hint({a: 1}).explain("executionStats");
printjson(explain2);
var stats2 = explain2.executionStats;
assert.eq(666, stats2.nReturned, 'wrong nReturned for explain2');
assert.eq(2000, stats2.totalKeysExamined, 'wrong totalKeysExamined for explain2');
