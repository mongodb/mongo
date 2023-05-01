(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_null_missing;
t.drop();

assert.commandWorked(t.insert({a: 2}));
assert.commandWorked(t.insert({a: {b: null}}));
assert.commandWorked(t.insert({a: {c: 1}}));

// Generate enough documents for index to be preferable.
for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: {b: i + 10}}));
}

{
    const res = t.explain("executionStats").aggregate([{$match: {'a.b': null}}]);
    assert.eq(3, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

assert.commandWorked(t.createIndex({'a.b': 1}));

{
    const res = t.explain("executionStats").aggregate([{$match: {'a.b': null}}]);
    assert.eq(3, res.executionStats.nReturned);
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}
}());
