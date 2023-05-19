(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_parallel_index;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 1000; i++) {
    bulk.insert({a: i});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(coll.createIndex({a: 1}));

let res = coll.explain("executionStats").aggregate([{$match: {a: {$lt: 10}}}]);
assert.eq(10, res.executionStats.nReturned);
assertValueOnPlanPath("IndexScan", res, "child.child.leftChild.child.nodeType");
}());
