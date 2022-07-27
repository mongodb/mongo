(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_array_match;
t.drop();

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.insert({a: 2, b: 1}));
    assert.commandWorked(t.insert({a: [2], b: 1}));
    assert.commandWorked(t.insert({a: [[2]], b: 1}));
    assert.commandWorked(t.insert({a: [0, 1], b: 1}));
}

assert.commandWorked(t.createIndex({a: 1}));

{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$eq: [2]}}}]);
    assert.eq(20, res.executionStats.nReturned);
    assert.eq("PhysicalScan", res.queryPlanner.winningPlan.optimizerPlan.child.child.nodeType);
}

{
    // These two predicates don't make a contradiction, because they can match different array
    // elements. Make sure we don't incorrectly simplify this to always-false.
    const res = t.explain("executionStats").aggregate([{$match: {a: 0}}, {$match: {a: 1}}]);
    assert.eq(10, res.executionStats.nReturned);
}

// Generate enough documents for index to be preferable.
const bulk = t.initializeUnorderedBulkOp();
for (let i = 0; i < 400; i++) {
    bulk.insert({a: i + 10});
}
assert.commandWorked(bulk.execute());

{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$eq: [2]}}}]);
    assert.eq(20, res.executionStats.nReturned);

    const indexUnionNode = res.queryPlanner.winningPlan.optimizerPlan.child.child.leftChild.child;
    assert.eq("Union", indexUnionNode.nodeType);
    assert.eq("IndexScan", indexUnionNode.children[0].nodeType);
    assert.eq([2], indexUnionNode.children[0].interval[0].lowBound.bound.value);
    assert.eq("IndexScan", indexUnionNode.children[1].nodeType);
    assert.eq(2, indexUnionNode.children[1].interval[0].lowBound.bound.value);
}

assert.commandWorked(t.dropIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1, a: 1}));

{
    const res = t.explain("executionStats").aggregate([{$match: {b: 1, a: {$eq: [2]}}}]);
    assert.eq(20, res.executionStats.nReturned);

    // Verify we still get index scan even if the field appears as second index field.
    const indexUnionNode = res.queryPlanner.winningPlan.optimizerPlan.child.child.leftChild.child;
    assert.eq("Union", indexUnionNode.nodeType);
    assert.eq("IndexScan", indexUnionNode.children[0].nodeType);
    assert.eq([2], indexUnionNode.children[0].interval[1].lowBound.bound.value);
    assert.eq("IndexScan", indexUnionNode.children[1].nodeType);
    assert.eq(2, indexUnionNode.children[1].interval[1].lowBound.bound.value);
}
}());
