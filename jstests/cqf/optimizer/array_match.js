import {
    assertValueOnPath,
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled,
    navigateToPlanPath,
    runWithFastPathsDisabled
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const t = db.cqf_array_match;
t.drop();
let docs = [];

for (let i = 0; i < 100; i++) {
    docs.push({a: 2, b: 1});
    docs.push({a: [2], b: 1});
    docs.push({a: [[2]], b: 1});
    docs.push({a: [0, 1], b: 1});
    docs.push({a: [], b: 1});
    docs.push({a: [3, []], b: 1});
}

assert.commandWorked(t.insertMany(docs));
assert.commandWorked(t.createIndex({a: 1}));

{
    const res = t.aggregate([{$match: {a: {$eq: [2]}}}]).toArray();
    assert.eq(200, res.length);
}
runWithFastPathsDisabled(() => {
    const res = t.explain("executionStats").aggregate([{$match: {a: {$eq: [2]}}}]);
    assert.eq(200, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
});

{
    // These two predicates don't make a contradiction, because they can match different array
    // elements. Make sure we don't incorrectly simplify this to always-false.
    const res = t.explain("executionStats").aggregate([{$match: {a: 0}}, {$match: {a: 1}}]);
    assert.eq(100, res.executionStats.nReturned);
}

assert.commandWorked(t.createIndex({a: 1}));

// Generate enough documents for index to be preferable.
assert.commandWorked(t.insertMany(Array.from({length: 5000}, (_, i) => ({a: [i + 10], b: 1}))));

{
    {
        const res = t.aggregate([{$match: {a: {$eq: [2]}}}]).toArray();
        assert.eq(200, res.length);
    }
    const res = runWithFastPathsDisabled(
        () => t.explain("executionStats").aggregate([{$match: {a: {$eq: [2]}}}]));
    assert.eq(200, res.executionStats.nReturned);

    const indexUnionNode = navigateToPlanPath(res, "child.child.leftChild.child");
    assertValueOnPath("SortedMerge", indexUnionNode, "nodeType");
    assertValueOnPath("IndexScan", indexUnionNode, "children.0.nodeType");
    assertValueOnPath([2], indexUnionNode, "children.0.interval.lowBound.bound.0.value");
    assertValueOnPath("IndexScan", indexUnionNode, "children.1.nodeType");
    assertValueOnPath(2, indexUnionNode, "children.1.interval.lowBound.bound.0.value");
}

{
    {
        const res = t.aggregate([{$match: {a: {$eq: []}}}]).toArray();
        assert.eq(200, res.length);
    }
    const res = runWithFastPathsDisabled(
        () => t.explain("executionStats").aggregate([{$match: {a: {$eq: []}}}]));
    assert.eq(200, res.executionStats.nReturned);

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
    assert.eq(200, res.executionStats.nReturned);

    // Verify we still get index scan even if the field appears as second index field.
    const indexUnionNode = navigateToPlanPath(res, "child.child.leftChild.child");
    assertValueOnPath("SortedMerge", indexUnionNode, "nodeType");
    assertValueOnPath("IndexScan", indexUnionNode, "children.0.nodeType");
    assertValueOnPath([2], indexUnionNode, "children.0.interval.lowBound.bound.1.value");
    assertValueOnPath("IndexScan", indexUnionNode, "children.1.nodeType");
    assertValueOnPath(2, indexUnionNode, "children.1.interval.lowBound.bound.1.value");
}
