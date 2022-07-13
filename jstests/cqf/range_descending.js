/*
 * Tests that descending indexes on range predicates yield all matching documents. Prior to the
 * fix in SERVER-67510, range predicates always scanned from low to high, even when the
 * corresponding index was descending. In this case, the optimizer would start from the "low" point
 * and continue until finding the "high" point, but because the index is descending, the "high"
 * point comes before the "low" point and is out of the index's search scope. To counteract this
 * behavior, the index bounds are swapped when the corresponding index is descending.
 */

(function() {
"use strict";

const coll = db.range_descending;
let query, res, explain;

coll.drop();

/*
 * This is the most basic case: a single range predicate with a descending index.
 */
assert.commandWorked(coll.insertOne({a: 1}));
assert.commandWorked(coll.createIndex({a: -1}));

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({}));
}

query = {
    a: {$gte: 0, $lte: 2}
};

res = coll.find(query).toArray();
assert.eq(res.length, 1);

explain = coll.explain("executionStats").find(query).finish();
assert.eq("IndexScan", explain.queryPlanner.winningPlan.optimizerPlan.child.leftChild.nodeType);

coll.drop();

/*
 * Test a compound index, with a range on the leading field and a descending index on the secondary
 * field.
 */
for (let i = 10; i <= 30; i += 10) {
    for (let j = 1; j <= 3; j++) {
        assert.commandWorked(coll.insert({a: i, b: j}));
    }
}
for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({}));
}
assert.commandWorked(coll.createIndex({a: 1, b: -1}));

query = {
    a: {$gte: 10, $lte: 20},
    b: {$gt: 1}
};

res = coll.find(query).toArray();
assert.eq(res.length, 4);

explain = coll.explain("executionStats").find(query).finish();
assert.eq("IndexScan",
          explain.queryPlanner.winningPlan.optimizerPlan.child.leftChild.child.nodeType);

coll.drop();

/*
 * Test a descending index with range predicates, ensuring that the index plan is chosen.
 */
assert.commandWorked(coll.insertOne({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: -1, b: -1}));

query = [{a: 1}, {_id: 0, a: 1, b: 1}];

res = coll.find(...query).toArray();
assert.eq(res.length, 1);

explain = coll.explain("executionStats").find(...query).finish();
assert.eq("IndexScan", explain.queryPlanner.winningPlan.optimizerPlan.child.child.nodeType);
}());