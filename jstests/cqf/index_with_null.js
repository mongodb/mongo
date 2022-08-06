(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_index_with_null;
t.drop();

for (let i = 0; i < 200; i++) {
    assert.commandWorked(t.insert({a: i, b: i, c: i}));
}
assert.commandWorked(t.insert([
    {_id: 1, a: 1, b: 1},
]));

t.createIndex({c: 1});

{
    const res = t.find({c: null}, {c: 1, _id: 0}).toArray();
    assert.eq(res, [{}]);
}

{
    const res = t.explain("executionStats").find({c: null}, {c: 1, _id: 0}).finish();
    assert.eq(1, res.executionStats.nReturned);

    // Verify the query **is not covered** by the index.
    assert.eq("Seek",
              res.queryPlanner.winningPlan.optimizerPlan.child.child.rightChild.child.nodeType);
    assert.eq("IndexScan",
              res.queryPlanner.winningPlan.optimizerPlan.child.child.leftChild.nodeType);
}

{
    const res = t.find({c: 3}, {c: 1, _id: 0}).toArray();
    assert.eq(res, [{c: 3}]);
}

{
    const res = t.explain("executionStats").find({c: 3}, {c: 1, _id: 0}).finish();
    assert.eq(1, res.executionStats.nReturned);

    // Verify the query **is covered** by the index.
    assert.eq("IndexScan", res.queryPlanner.winningPlan.optimizerPlan.child.child.nodeType);
}
}());
