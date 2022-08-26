(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_index_intersect1;
t.drop();

assert.commandWorked(t.insert({a: 50}));
assert.commandWorked(t.insert({a: 70}));
assert.commandWorked(t.insert({a: 90}));
assert.commandWorked(t.insert({a: 110}));
assert.commandWorked(t.insert({a: 130}));

// Generate enough documents for index to be preferable.
for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: 200 + i}));
}

assert.commandWorked(t.createIndex({'a': 1}));

let res = t.explain("executionStats").aggregate([{$match: {'a': {$gt: 60, $lt: 100}}}]);
assert.eq(2, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");

// Should get a covered plan.
res = t.explain("executionStats")
          .aggregate([{$project: {'_id': 0, 'a': 1}}, {$match: {'a': {$gt: 60, $lt: 100}}}]);
assert.eq(2, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.child.nodeType");
}());