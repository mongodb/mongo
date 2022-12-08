(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_recursive_ix_nav;
t.drop();

const valueRange = 20;
const duplicates = 5;

const bulk = t.initializeUnorderedBulkOp();
for (let i = 0; i < valueRange; i++) {
    for (let j = 0; j < valueRange; j++) {
        for (let k = 0; k < valueRange; k++) {
            for (let l = 0; l < duplicates; l++) {
                bulk.insert({a: i, b: j, c: k});
            }
        }
    }
}
assert.commandWorked(bulk.execute());

assert.commandWorked(t.createIndex({a: 1, b: 1, c: 1}));

const res = runWithParams(
    [
        {key: "internalCascadesOptimizerMinIndexEqPrefixes", value: 2},
        {key: "internalCascadesOptimizerMaxIndexEqPrefixes", value: 2}
    ],
    () => t.explain("executionStats").aggregate([{$match: {a: {$eq: 1}, c: {$eq: 3}}}]));
assert.eq(100, res.executionStats.nReturned);

assertValueOnPlanPath("IndexScan", res, "child.leftChild.leftChild.child.nodeType");
assertValueOnPlanPath("IndexScan", res, "child.leftChild.rightChild.nodeType");
}());
