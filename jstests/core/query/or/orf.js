// Test a query with 200 $or clauses
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
//   # `explain.executionStats` is not causally consistent.
//   does_not_support_causal_consistency,
// ]

(function() {
'use strict';

load("jstests/libs/clustered_collections/clustered_collection_util.js");

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
const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());
assert.eq(200, explain.executionStats.nReturned, 'n');
const expectedKeysExamined = collectionIsClustered ? 0 : 200;
assert.eq(expectedKeysExamined, explain.executionStats.totalKeysExamined, 'keys examined');
assert.eq(200, explain.executionStats.totalDocsExamined, 'docs examined');

assert.eq(200, t.count({$or: a}));
})();
