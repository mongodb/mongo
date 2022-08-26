(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_exchange;
t.drop();

assert.commandWorked(t.insert({a: 1}));
assert.commandWorked(t.insert({a: 2}));
assert.commandWorked(t.insert({a: 3}));
assert.commandWorked(t.insert({a: 4}));
assert.commandWorked(t.insert({a: 5}));

// Demonstrate local-global optimization.
const res = t.explain("executionStats").aggregate([{$group: {_id: "$a", cnt: {$sum: 1}}}]);
assert.eq(5, res.executionStats.nReturned);

assertValueOnPlanPath("Exchange", res, "child.nodeType");
assertValueOnPlanPath("Centralized", res, "child.distribution.type");

assertValueOnPlanPath("GroupBy", res, "child.child.child.nodeType");

assertValueOnPlanPath("Exchange", res, "child.child.child.child.nodeType");
assertValueOnPlanPath("HashPartitioning", res, "child.child.child.child.distribution.type");

assertValueOnPlanPath("GroupBy", res, "child.child.child.child.child.nodeType");
assertValueOnPlanPath(
    "UnknownPartitioning",
    res,
    "child.child.child.child.child.properties.physicalProperties.distribution.type");
}());