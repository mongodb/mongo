(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_value_elemMatch;
t.drop();

assert.commandWorked(t.insert({a: [1, 2, 3, 4, 5, 6]}));
assert.commandWorked(t.insert({a: [5, 6, 7, 8, 9]}));
assert.commandWorked(t.insert({a: [1, 2, 3]}));
assert.commandWorked(t.insert({a: []}));
assert.commandWorked(t.insert({a: [1]}));
assert.commandWorked(t.insert({a: [10]}));
assert.commandWorked(t.insert({a: 5}));
assert.commandWorked(t.insert({a: 6}));

// Generate enough documents for index to be preferable.
const nDocs = 400;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(t.insert({a: i + 10}));
}

assert.commandWorked(t.createIndex({a: 1}));

{
    // Value elemMatch. Demonstrate we can use an index.
    const res =
        t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {$gte: 5, $lte: 6}}}}]);
    assert.eq(2, res.executionStats.nReturned);
    assertValueOnPlanPath("IndexScan", res, "child.child.leftChild.child.nodeType");
}
{
    const res =
        t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {$lt: 11, $gt: 9}}}}]);
    assert.eq(1, res.executionStats.nReturned);
    assertValueOnPlanPath("IndexScan", res, "child.child.leftChild.child.nodeType");
}
{
    // Contradiction.
    const res =
        t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {$lt: 5, $gt: 6}}}}]);
    assert.eq(0, res.executionStats.nReturned);
    assertValueOnPlanPath("CoScan", res, "child.child.child.nodeType");
}
}());
