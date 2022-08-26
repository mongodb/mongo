(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_index_intersect;
t.drop();

const nMatches = 60;

assert.commandWorked(t.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(t.insert({a: 3, b: 2, c: 1}));
for (let i = 0; i < nMatches; i++) {
    assert.commandWorked(t.insert({a: 3, b: 3, c: i}));
}
assert.commandWorked(t.insert({a: 4, b: 3, c: 2}));
assert.commandWorked(t.insert({a: 5, b: 5, c: 2}));

for (let i = 1; i < nMatches + 100; i++) {
    assert.commandWorked(t.insert({a: i + nMatches, b: i + nMatches, c: i + nMatches}));
}

assert.commandWorked(t.createIndex({'a': 1}));
assert.commandWorked(t.createIndex({'b': 1}));

let res = t.explain("executionStats").aggregate([{$match: {'a': 3, 'b': 3}}]);
assert.eq(nMatches, res.executionStats.nReturned);

// Verify we can place a MergeJoin
let joinNode = navigateToPlanPath(res, "child.leftChild");
assertValueOnPath("MergeJoin", joinNode, "nodeType");
assertValueOnPath("IndexScan", joinNode, "leftChild.nodeType");
assertValueOnPath("IndexScan", joinNode, "rightChild.children.0.child.nodeType");

// One side is not equality, and we use a HashJoin.
res = t.explain("executionStats").aggregate([{$match: {'a': {$lte: 3}, 'b': 3}}]);
assert.eq(nMatches, res.executionStats.nReturned);

joinNode = navigateToPlanPath(res, "child.leftChild");
assertValueOnPath("HashJoin", joinNode, "nodeType");
assertValueOnPath("IndexScan", joinNode, "leftChild.nodeType");
assertValueOnPath("IndexScan", joinNode, "rightChild.children.0.child.nodeType");
}());
