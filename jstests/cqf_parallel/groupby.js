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

assert.eq("Exchange", res.queryPlanner.winningPlan.optimizerPlan.child.nodeType);
assert.eq("Centralized", res.queryPlanner.winningPlan.optimizerPlan.child.distribution.type);

assert.eq("GroupBy", res.queryPlanner.winningPlan.optimizerPlan.child.child.child.nodeType);

assert.eq("Exchange", res.queryPlanner.winningPlan.optimizerPlan.child.child.child.child.nodeType);
assert.eq("HashPartitioning",
          res.queryPlanner.winningPlan.optimizerPlan.child.child.child.child.distribution.type);

assert.eq("GroupBy",
          res.queryPlanner.winningPlan.optimizerPlan.child.child.child.child.child.nodeType);
assert.eq("UnknownPartitioning",
          res.queryPlanner.winningPlan.optimizerPlan.child.child.child.child.child.properties
              .physicalProperties.distribution.type);
}());