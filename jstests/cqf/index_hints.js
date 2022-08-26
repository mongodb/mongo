(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_index_hints;
t.drop();

assert.commandWorked(t.insert({a: [1, 2, 3, 4]}));
assert.commandWorked(t.insert({a: [2, 3, 4]}));
assert.commandWorked(t.insert({a: [2]}));
assert.commandWorked(t.insert({a: 2}));
assert.commandWorked(t.insert({a: [1, 3]}));

assert.commandWorked(t.createIndex({a: 1}));

// There are too few documents, and an index is not preferable.
{
    let res = t.explain("executionStats").find({a: 2}).finish();
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

{
    let res = t.explain("executionStats").find({a: 2}).hint({a: 1}).finish();
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}

{
    let res = t.explain("executionStats").find({a: 2}).hint("a_1").finish();
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}

{
    let res = t.explain("executionStats").find({a: 2}).hint({$natural: 1}).finish();
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

// Generate enough documents for index to be preferable.
for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: i + 10}));
}

{
    let res = t.explain("executionStats").find({a: 2}).finish();
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}

{
    let res = t.explain("executionStats").find({a: 2}).hint({a: 1}).finish();
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}

{
    let res = t.explain("executionStats").find({a: 2}).hint("a_1").finish();
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}
{
    let res = t.explain("executionStats").find({a: 2}).hint({$natural: 1}).finish();
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}
}());
