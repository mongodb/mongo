/**
 * Tests scenario related to SERVER-12923.
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sort_match;
t.drop();

const bulk = t.initializeUnorderedBulkOp();
const nDocs = 1000;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({a: i, b: i % 10});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1}));

let res = t.explain("executionStats").aggregate([{$sort: {b: 1}}, {$match: {a: {$eq: 0}}}]);
assert.eq(1, res.executionStats.nReturned);

// Index on "a" is preferred.
const indexNode = navigateToPlanPath(res, "child.child.leftChild");
assertValueOnPath("IndexScan", indexNode, "nodeType");
assertValueOnPath("a_1", indexNode, "indexDefName");
}());