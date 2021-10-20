// Check explain results for a plan that uses an index to obtain the requested sort order.
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
// ]

(function() {
'use strict';

const t = db.jstests_explain5;
t.drop();

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1}));

for (let i = 0; i < 1000; ++i) {
    assert.commandWorked(t.insert({_id: i, a: i, b: i % 3}));
}

// Query with an initial set of documents.
const explain1 = t.find({a: {$gte: 0}, b: 2}).sort({a: 1}).hint({a: 1}).explain("executionStats");
jsTestLog('explain5 explain output after initial documents: ' + tojson(explain1));
const stats1 = explain1.executionStats;
assert.eq(333, stats1.nReturned, 'wrong nReturned for explain1');
assert.eq(1000, stats1.totalKeysExamined, 'wrong totalKeysExamined for explain1');

for (let i = 1000; i < 2000; ++i) {
    assert.commandWorked(t.insert({_id: i, a: i, b: i % 3}));
}

// Query with some additional documents.
const explain2 = t.find({a: {$gte: 0}, b: 2}).sort({a: 1}).hint({a: 1}).explain("executionStats");
jsTestLog('explain5 explain output after additional documents: ' + tojson(explain2));
const stats2 = explain2.executionStats;
assert.eq(666, stats2.nReturned, 'wrong nReturned for explain2');
assert.eq(2000, stats2.totalKeysExamined, 'wrong totalKeysExamined for explain2');
})();
