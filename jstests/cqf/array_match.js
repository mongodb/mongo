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
    assert.commandWorked(t.insert({a: [], b: 1}));
    assert.commandWorked(t.insert({a: [3, []], b: 1}));
}

assert.commandWorked(t.createIndex({a: 1}));

{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$eq: [2]}}}]);
    assert.eq(20, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
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

    const indexUnionNode = navigateToPlanPath(res, "child.child.leftChild.child");
    assertValueOnPath("SortedMerge", indexUnionNode, "nodeType");
    assertValueOnPath("IndexScan", indexUnionNode, "children.0.nodeType");
    assertValueOnPath([2], indexUnionNode, "children.0.interval.lowBound.bound.0.value");
    assertValueOnPath("IndexScan", indexUnionNode, "children.1.nodeType");
    assertValueOnPath(2, indexUnionNode, "children.1.interval.lowBound.bound.0.value");
}

{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$eq: []}}}]);
    assert.eq(20, res.executionStats.nReturned);
    const indexUnionNode = navigateToPlanPath(res, "child.child.leftChild.child");
    assertValueOnPath("SortedMerge", indexUnionNode, "nodeType");
    assertValueOnPath("IndexScan", indexUnionNode, "children.0.nodeType");
    assertValueOnPath(undefined, indexUnionNode, "children.0.interval.lowBound.bound.0.value");
    assertValueOnPath("IndexScan", indexUnionNode, "children.1.nodeType");
    assertValueOnPath([], indexUnionNode, "children.1.interval.lowBound.bound.0.value");
}

assert.commandWorked(t.dropIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1, a: 1}));

{
    const res = t.explain("executionStats").aggregate([{$match: {b: 1, a: {$eq: [2]}}}]);
    assert.eq(20, res.executionStats.nReturned);

    // Verify we still get index scan even if the field appears as second index field.
    const indexUnionNode = navigateToPlanPath(res, "child.child.leftChild.child");
    assertValueOnPath("SortedMerge", indexUnionNode, "nodeType");
    assertValueOnPath("IndexScan", indexUnionNode, "children.0.nodeType");
    assertValueOnPath([2], indexUnionNode, "children.0.interval.lowBound.bound.1.value");
    assertValueOnPath("IndexScan", indexUnionNode, "children.1.nodeType");
    assertValueOnPath(2, indexUnionNode, "children.1.interval.lowBound.bound.1.value");
}
}());
