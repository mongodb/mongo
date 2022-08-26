/**
 * Tests scenario related to SERVER-20616.
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_selective_index;
t.drop();

const bulk = t.initializeUnorderedBulkOp();
const nDocs = 1000;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({a: i % 10, b: i});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1}));

// Predicate on "b" is more selective than the one on "a": 0.1% vs 10%.
const res = t.explain("executionStats").aggregate([{$match: {a: {$eq: 0}, b: {$eq: 0}}}]);
assert.eq(1, res.executionStats.nReturned);

// Demonstrate we can pick index on "b".
const indexNode = navigateToPlanPath(res, "child.leftChild");
assertValueOnPath("IndexScan", indexNode, "nodeType");
assertValueOnPath("b_1", indexNode, "indexDefName");
}());