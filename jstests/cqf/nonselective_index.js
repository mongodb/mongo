/**
 * Tests scenario related to SERVER-13065.
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_nonselective_index;
t.drop();

const bulk = t.initializeUnorderedBulkOp();
const nDocs = 1000;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({a: i});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(t.createIndex({a: 1}));

// We pick collection scan since the query is not selective.
const res = t.explain("executionStats").aggregate([{$match: {a: {$gte: 0}}}]);
assert.eq(nDocs, res.executionStats.nReturned);

assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}());