(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_array_index;
t.drop();

assert.commandWorked(t.insert({a: [1, 2, 3, 4]}));
assert.commandWorked(t.insert({a: [2, 3, 4]}));
assert.commandWorked(t.insert({a: [2]}));
assert.commandWorked(t.insert({a: 2}));
assert.commandWorked(t.insert({a: [1, 3]}));

// Generate enough documents for index to be preferable.
for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: i + 10}));
}

assert.commandWorked(t.createIndex({a: 1}));

let res = t.explain("executionStats").aggregate([{$match: {a: 2}}]);
assert.eq(4, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");

res = t.explain("executionStats").aggregate([{$match: {a: {$lt: 2}}}]);
assert.eq(2, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.child.nodeType");
}());
