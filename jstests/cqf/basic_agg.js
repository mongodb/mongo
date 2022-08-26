(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_basic_index;
coll.drop();

assert.commandWorked(
    coll.insert([{a: {b: 1}}, {a: {b: 2}}, {a: {b: 3}}, {a: {b: 4}}, {a: {b: 5}}]));

const extraDocCount = 50;
// Add extra docs to make sure indexes can be picked.
for (let i = 0; i < extraDocCount; i++) {
    assert.commandWorked(coll.insert({a: {b: i + 10}}));
}
assert.commandWorked(coll.createIndex({'a.b': 1}));

let res = coll.explain("executionStats").aggregate([{$match: {'a.b': 2}}]);
assert.eq(1, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");

res = coll.explain("executionStats").aggregate([{$match: {'a.b': {$gt: 2}}}]);
assert.eq(3 + extraDocCount, res.executionStats.nReturned);
assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");

res = coll.explain("executionStats").aggregate([{$match: {'a.b': {$gte: 2}}}]);
assert.eq(4 + extraDocCount, res.executionStats.nReturned);
assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");

res = coll.explain("executionStats").aggregate([{$match: {'a.b': {$lt: 2}}}]);
assert.eq(1, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");

res = coll.explain("executionStats").aggregate([{$match: {'a.b': {$lte: 2}}}]);
assert.eq(2, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}());
