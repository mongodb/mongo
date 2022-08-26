(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_exchange;
t.drop();

assert.commandWorked(t.insert({a: {b: 1}}));
assert.commandWorked(t.insert({a: {b: 2}}));
assert.commandWorked(t.insert({a: {b: 3}}));
assert.commandWorked(t.insert({a: {b: 4}}));
assert.commandWorked(t.insert({a: {b: 5}}));

const res = t.explain("executionStats").aggregate([{$match: {'a.b': 2}}]);
assert.eq(1, res.executionStats.nReturned);
assertValueOnPlanPath("Exchange", res, "child.nodeType");
}());
