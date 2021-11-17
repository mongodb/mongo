// Test a query with 200 $or clauses
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
// ]

(function() {
'use strict';

const t = db.jstests_orf;
t.drop();

let a = [];
for (let i = 0; i < 200; ++i) {
    a.push({_id: i});
}
assert.commandWorked(t.insert(a));

// This $or query is answered as an index scan over
// a series of _id index point intervals.
const explain = t.find({$or: a}).hint({_id: 1}).explain(true);
printjson(explain);
assert.eq(200, explain.executionStats.nReturned, 'n');
assert.eq(200, explain.executionStats.totalKeysExamined, 'keys examined');
assert.eq(200, explain.executionStats.totalDocsExamined, 'docs examined');

assert.eq(200, t.count({$or: a}));
})();
