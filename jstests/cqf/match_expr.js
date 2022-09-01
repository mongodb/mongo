(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_match_expr;
t.drop();

const bulk = t.initializeUnorderedBulkOp();
for (let va = 0; va < 5; va++) {
    for (let vb = 0; vb < 5; vb++) {
        for (let vc = 0; vc < 5; vc++) {
            bulk.insert({a: va, b: vb, c: vc});
        }
    }
}
assert.commandWorked(bulk.execute());

const expr = {
    $expr: {
        $or: [
            {$and: [{$eq: ["$a", 1]}, {$eq: ["$b", 2]}]},
            {$eq: ["$c", 3]},
        ]
    }
};

const numExpected = 1 * 5 * 5 + 4 * 1 * 1;

{
    const res = t.explain("executionStats").aggregate([{$match: expr}]);
    assert.eq(numExpected, res.executionStats.nReturned);

    assertValueOnPlanPath("Filter", res, "child.nodeType");
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

{
    const res = t.explain("executionStats").find(expr).finish();
    assert.eq(numExpected, res.executionStats.nReturned);

    assertValueOnPlanPath("Filter", res, "child.nodeType");
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}
}());
