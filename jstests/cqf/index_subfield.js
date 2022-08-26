(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_index_subfield;
t.drop();

assert.commandWorked(t.insert({a: 1, b: 1}));
for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.insert({a: 2, b: {c: 2}}));
    assert.commandWorked(t.insert({a: 3, b: {c: 3}}));
}
assert.commandWorked(t.insert({a: 5, b: {d: 5}}));

for (let i = 0; i < 200; i++) {
    assert.commandWorked(t.insert({a: i + 10, b: {c: i + 10}}));
}

assert.commandWorked(t.createIndex({a: 1, b: 1}));

{
    // Assert we have a covered query.
    const res =
        t.explain("executionStats").find({a: 2, 'b.c': 3}, {_id: 0, a: 1}).hint("a_1_b_1").finish();
    assertValueOnPlanPath("IndexScan", res, "child.child.child.nodeType");
}
}());
