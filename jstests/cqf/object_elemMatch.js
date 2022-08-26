(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_object_elemMatch;

t.drop();
assert.commandWorked(t.insert({a: [{a: 1, b: 1}, {a: 1, b: 2}]}));
assert.commandWorked(t.insert({a: [{a: 2, b: 1}, {a: 2, b: 2}]}));
assert.commandWorked(t.insert({a: {a: 2, b: 1}}));
assert.commandWorked(t.insert({a: [{b: [1, 2], c: [3, 4]}]}));

{
    // Object elemMatch. Currently we do not support index here.
    const res = t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {a: 2, b: 1}}}}]);
    assert.eq(1, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.child.nodeType");
}

{
    // Should not be getting any results.
    const res = t.explain("executionStats").aggregate([
        {$match: {a: {$elemMatch: {b: {$elemMatch: {}}, c: {$elemMatch: {}}}}}}
    ]);
    assert.eq(0, res.executionStats.nReturned);
}
}());
