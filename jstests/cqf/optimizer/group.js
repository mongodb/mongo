(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_group;
coll.drop();

assert.commandWorked(coll.insert([
    {a: 1, b: 1, c: 1},
    {a: 1, b: 2, c: 2},
    {a: 1, b: 2, c: 3},
    {a: 2, b: 1, c: 4},
    {a: 2, b: 1, c: 5},
    {a: 2, b: 2, c: 6},
]));

const res = coll.explain("executionStats").aggregate([
    {$group: {_id: {a: '$a', b: '$b'}, sum: {$sum: '$c'}, avg: {$avg: '$c'}}}
]);
assertValueOnPlanPath("GroupBy", res, "child.child.nodeType");
assert.eq(4, res.executionStats.nReturned);
}());
