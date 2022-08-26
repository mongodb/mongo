(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_basic_unwind;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 1},
    {_id: 2, x: null},
    {_id: 3, x: []},
    {_id: 4, x: [1, 2]},
    {_id: 5, x: [3]},
    {_id: 6, x: 4}
]));

let res = coll.explain("executionStats").aggregate([{$unwind: '$x'}]);
assert.eq(4, res.executionStats.nReturned);
assertValueOnPlanPath("Unwind", res, "child.child.nodeType");
}());
