import {
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled,
    runWithFastPathsDisabled,
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const coll = db.cqf_basic_find;
coll.drop();

const docs = [{a: {b: 1}}, {a: {b: 2}}, {a: {b: 3}}, {a: {b: 4}}, {a: {b: 5}}, {'': 3}];

const extraDocCount = 500;
// Add extra docs to make sure indexes can be picked.
for (let i = 0; i < extraDocCount; i++) {
    docs.push({a: {b: i + 10}});
}

assert.commandWorked(coll.insertMany(docs));

assert.commandWorked(coll.createIndex({'a.b': 1}));

let res = coll.explain("executionStats").find({'a.b': 2}).finish();
assert.eq(1, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");

res = coll.explain("executionStats").find({'a.b': {$gt: 2}}).finish();
assert.eq(3 + extraDocCount, res.executionStats.nReturned);
assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");

res = coll.explain("executionStats").find({'a.b': {$gte: 2}}).finish();
assert.eq(4 + extraDocCount, res.executionStats.nReturned);
assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");

res = coll.explain("executionStats").find({'a.b': {$lt: 2}}).finish();
assert.eq(1, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");

res = coll.explain("executionStats").find({'a.b': {$lte: 2}}).finish();
assert.eq(2, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");

res = coll.explain("executionStats").find({'': {$gt: 2}}).finish();
assert.eq(1, res.executionStats.nReturned);

res = coll.find({'': {$gt: 2}}).toArray();
assert.eq(1, res.length);
res = runWithFastPathsDisabled(() => coll.explain("executionStats").find({'': {$gt: 2}}).finish());
assert.eq(1, res.executionStats.nReturned);
assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
