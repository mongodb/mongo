(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sort_compound_pred;
t.drop();

const documents = [];
for (let i = 0; i < 100; i++) {
    for (let j = 0; j < 10; j++) {
        documents.push({a: i});
    }
}
assert.commandWorked(t.insertMany(documents));
assert.commandWorked(t.createIndex({a: 1}));

{
    const res = t.explain("executionStats").aggregate([{$match: {$or: [{a: 1}, {a: 2}]}}]);
    assert.eq(20, res.executionStats.nReturned);

    // No collation node on the path (we are not sorting).
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.child.children.0.nodeType");
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.child.children.1.nodeType");
}

{
    const res =
        t.explain("executionStats").aggregate([{$match: {$or: [{a: 1}, {a: 2}]}}, {$sort: {a: 1}}]);
    assert.eq(20, res.executionStats.nReturned);

    // Collation node on the path. It is not subsumed in the index scan because we have a compound
    // predicate.
    assertValueOnPlanPath("Collation", res, "child.nodeType");
    assertValueOnPlanPath("IndexScan", res, "child.child.leftChild.child.children.0.nodeType");
    assertValueOnPlanPath("IndexScan", res, "child.child.leftChild.child.children.1.nodeType");
}
}());
