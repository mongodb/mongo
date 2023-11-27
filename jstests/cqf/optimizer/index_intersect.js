import {
    assertValueOnPath,
    checkCascadesOptimizerEnabled,
    navigateToPlanPath,
    runWithParams,
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const t = db.cqf_index_intersect;
t.drop();

const documents = [{a: 1, b: 1, c: 1}, {a: 3, b: 2, c: 1}];
const nMatches = 500;
for (let i = 0; i < nMatches; i++) {
    documents.push({a: 3, b: 3, c: i});
}

// Because a and b are both indexed fields and they have the same value in most documents, the
// optimal plan will be to estimate their cardinality together due to their high correlation.
// Instead, add around 12000 documents where a and b are not 3 to encourage a disjoint IndexScan.
for (let i = 4; i < 4000; i++) {
    documents.push({a: 3, b: i, c: i});
    documents.push({a: i, b: 3, c: i});
    documents.push({a: i + nMatches, b: i + nMatches, c: i + nMatches});
}

assert.commandWorked(t.insertMany(documents));

assert.commandWorked(t.createIndex({'a': 1}));
assert.commandWorked(t.createIndex({'b': 1}));

// TODO SERVER-71553 The Cost Model is overriden to preserve MergeJoin plan.
// In majority of cases it works well without Cost Model override, but in some rare cases it fails.
let res = runWithParams([{
                            key: 'internalCostModelCoefficients',
                            value: {"mergeJoinStartupCost": 1e-9, "mergeJoinIncrementalCost": 1e-9}
                        }],
                        () => t.explain("executionStats").aggregate([{$match: {'a': 3, 'b': 3}}]));
assert.eq(nMatches, res.executionStats.nReturned);

// Verify we can place a MergeJoin
let joinNode = navigateToPlanPath(res, "child.leftChild");
assertValueOnPath("MergeJoin", joinNode, "nodeType");
assertValueOnPath("IndexScan", joinNode, "leftChild.nodeType");
assertValueOnPath("IndexScan", joinNode, "rightChild.children.0.child.nodeType");

// One side is not equality, and we use a HashJoin.
// TODO SERVER-71553 The Cost Model is overriden to preserve HashJoin plan.
res = runWithParams(
    [{key: 'internalCostModelCoefficients', value: {"hashJoinIncrementalCost": 1e-9}}],
    () => t.explain("executionStats").aggregate([{$match: {'a': {$lte: 3}, 'b': 3}}]));
assert.eq(nMatches, res.executionStats.nReturned);

joinNode = navigateToPlanPath(res, "child.leftChild");
assertValueOnPath("HashJoin", joinNode, "nodeType");
assertValueOnPath("IndexScan", joinNode, "leftChild.nodeType");
assertValueOnPath("IndexScan", joinNode, "rightChild.children.0.child.nodeType");
