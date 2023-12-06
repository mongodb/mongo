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

for (let i = 0; i < 10; i++) {
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
    assert.eq(20, res.length);
}
runWithFastPathsDisabled(() => {
    const res = t.explain("executionStats").aggregate([{$match: {a: {$eq: [2]}}}]);
    assert.eq(20, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
});

{
    // These two predicates don't make a contradiction, because they can match different array
    // elements. Make sure we don't incorrectly simplify this to always-false.
    const res = t.explain("executionStats").aggregate([{$match: {a: 0}}, {$match: {a: 1}}]);
    assert.eq(10, res.executionStats.nReturned);
}

t.drop();
assert.commandWorked(t.createIndex({a: 1}));

// Generate enough documents for index to be preferable.
let newDocs = Array.from({length: 1060}, (_, i) => ({a: i + 10}));

// Distribute interesting documents to encourage IndexScan when sampling in chunks.
for (let i = 0; i < docs.length; i++) {
    const idx = Math.floor(i * (newDocs.length / docs.length));
    newDocs[idx] = docs[i];
}
assert.commandWorked(t.insertMany(newDocs));

{
    {
        const res = t.aggregate([{$match: {a: {$eq: [2]}}}]).toArray();
        assert.eq(20, res.length);
    }
    const res = runWithFastPathsDisabled(
        () => t.explain("executionStats").aggregate([{$match: {a: {$eq: [2]}}}]));
    assert.eq(20, res.executionStats.nReturned);

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
        assert.eq(20, res.length);
    }
    const res = runWithFastPathsDisabled(
        () => t.explain("executionStats").aggregate([{$match: {a: {$eq: []}}}]));
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
